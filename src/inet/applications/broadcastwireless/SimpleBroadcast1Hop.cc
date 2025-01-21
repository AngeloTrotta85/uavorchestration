//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
// 
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/.
// 

#include "SimpleBroadcast1Hop.h"

#include "inet/common/packet/Packet.h"

#include "inet/applications/base/ApplicationPacket_m.h"
#include "inet/common/ModuleAccess.h"
#include "inet/common/TagBase_m.h"
#include "inet/common/TimeTag_m.h"
#include "inet/common/lifecycle/ModuleOperations.h"
#include "inet/common/packet/Packet.h"
#include "inet/networklayer/common/FragmentationTag_m.h"
#include "inet/networklayer/common/L3AddressResolver.h"
#include "inet/transportlayer/contract/udp/UdpControlInfo_m.h"

#include "inet/common/ProtocolGroup.h"
#include "inet/common/ProtocolTag_m.h"
#include "inet/common/lifecycle/ModuleOperations.h"
#include "inet/common/lifecycle/NodeStatus.h"
#include "inet/networklayer/common/L3AddressResolver.h"
#include "inet/networklayer/common/L3AddressTag_m.h"
#include "inet/transportlayer/common/L4PortTag_m.h"
#include "inet/common/ModuleAccess.h"

namespace inet {

Define_Module(SimpleBroadcast1Hop);

SimpleBroadcast1Hop::~SimpleBroadcast1Hop()
{
    cancelAndDelete(selfMsg);
    cancelAndDelete(taskMsg);
    //test
}

void SimpleBroadcast1Hop::initialize(int stage)
{
    ClockUserModuleMixin::initialize(stage);

    if (stage == INITSTAGE_LOCAL) {
        numSent = 0;
        numReceived = 0;
        WATCH(numSent);
        WATCH(numReceived);

        WATCH_MAP(nodeDataMap);

        localPort = par("localPort");
        destPort = par("destPort");
        startTime = par("startTime");
        stopTime = par("stopTime");
        packetName = par("packetName");
        dontFragment = par("dontFragment");

        computationalPower = par("computationalPower");
        availableMaxMemory = par("availableMaxMemory");
        hasCamera = par("hasCamera").boolValue();
        hasGPU = par("hasGPU").boolValue();

        if (stopTime >= CLOCKTIME_ZERO && stopTime < startTime)
            throw cRuntimeError("Invalid startTime/stopTime parameters");
        selfMsg = new ClockEvent("sendTimer");
        taskMsg = new ClockEvent("taskTimer");
    }
    else if (stage == INITSTAGE_LAST){
        mob = check_and_cast<IMobility *>(this->getParentModule()->getSubmodule("mobility"));

        EV_INFO << "Node position is: " << mob->getCurrentPosition() << endl;

        // Obtain the IP address
        myAddress = L3AddressResolver().addressOf(getParentModule(), L3AddressResolver::ADDR_IPv4);

        // Log or store the IP address
        EV_INFO << "My IP address is: " << myAddress.str() << endl;

    }
}

void SimpleBroadcast1Hop::finish()
{
    recordScalar("packets sent", numSent);
    recordScalar("packets received", numReceived);
    EV_INFO << myAddress.str() << " table size: " << nodeDataMap.size() << " Stack size: " << stChanges.size() << endl;
    ApplicationBase::finish();
}

void SimpleBroadcast1Hop::setSocketOptions()
{
    int timeToLive = par("timeToLive");
    if (timeToLive != -1)
        socket.setTimeToLive(timeToLive);

    int dscp = par("dscp");
    if (dscp != -1)
        socket.setDscp(dscp);

    int tos = par("tos");
    if (tos != -1)
        socket.setTos(tos);

    const char *multicastInterface = par("multicastInterface");
    if (multicastInterface[0]) {
        IInterfaceTable *ift = getModuleFromPar<IInterfaceTable>(par("interfaceTableModule"), this);
        NetworkInterface *ie = ift->findInterfaceByName(multicastInterface);
        if (!ie)
            throw cRuntimeError("Wrong multicastInterface setting: no interface named \"%s\"", multicastInterface);
        socket.setMulticastOutputInterface(ie->getInterfaceId());
    }

    bool receiveBroadcast = par("receiveBroadcast");
    if (receiveBroadcast)
        socket.setBroadcast(true);

    bool joinLocalMulticastGroups = par("joinLocalMulticastGroups");
    if (joinLocalMulticastGroups) {
        MulticastGroupList mgl = getModuleFromPar<IInterfaceTable>(par("interfaceTableModule"), this)->collectMulticastGroups();
        socket.joinLocalMulticastGroups(mgl);
    }
    socket.setCallback(this);
}

L3Address SimpleBroadcast1Hop::chooseDestAddr()
{
    int k = intrand(destAddresses.size());
    if (destAddresses[k].isUnspecified() || destAddresses[k].isLinkLocal()) {
        L3AddressResolver().tryResolve(destAddressStr[k].c_str(), destAddresses[k]);
    }
    return destAddresses[k];
}

Ptr<ChangesBlock> SimpleBroadcast1Hop::createPayload()
{

    //update NodeInfo with assigned tasks

    double compActUsage = 0;
    double memActUsage = 0;
    bool lockedGPU = false;
    bool lockedCamera = false;
    bool lockedFly = false;
    for (size_t i = 0; i < assignedTask_list.size(); ++i) {
        if ((simTime() < assignedTask_list[i].end_timestamp) && (simTime() >= assignedTask_list[i].start_timestamp)){
            compActUsage += assignedTask_list[i].req_computation;
            memActUsage += assignedTask_list[i].req_memory;
            if (assignedTask_list[i].req_lock_GPU) lockedGPU = true;
            if (assignedTask_list[i].req_lock_camera) lockedCamera = true;
            if (assignedTask_list[i].req_lock_flyengine) lockedFly = true;
        }
    }


    const auto& payload = makeShared<ChangesBlock>();

    //getting changes of THIS node...

    uint32_t seq = lastReport.getSequenceNumber() + 1;

    double deltaComp = 5; // acceptable delta without notification
    double deltaMemory = 5;
    double deltaCoord = 2; // percent (%)

    //for debug:
    bool go = true; //sent my own data even though noting has changed

    //comparing actual values with last reported ones over deltas
    Change ch;
    ch.setSequenceNumber(seq);
    ch.setIpAddress(myAddress);
    ch.setNum_hops(0);
    int stksize = stChanges.size();
    if ((abs((lastReport.getCoord_x() - mob->getCurrentPosition().x)/lastReport.getCoord_x()) * 100) > deltaCoord ||
            (abs((lastReport.getCoord_y() - mob->getCurrentPosition().y)/lastReport.getCoord_y()) * 100)  > deltaCoord  || go) {
        //report new position
        ch.setParammeter(fldPOS_x);
        ch.setValue(mob->getCurrentPosition().x);
        lastReport.setCoord_x(mob->getCurrentPosition().x);
        stChanges.push_back(ch);

        ch.setParammeter(fldPOS_y);
        ch.setValue(mob->getCurrentPosition().y);
        lastReport.setCoord_y(mob->getCurrentPosition().y);
        stChanges.push_back(ch);

    }

    if (lastReport.getCompMaxUsage() != computationalPower  || go) {
        //report Max CPU
        ch.setParammeter(fldMaxCPU);
        ch.setValue(computationalPower);
        lastReport.setCompMaxUsage(computationalPower);
        stChanges.push_back(ch);
    }

    if (lastReport.getMemoryMaxUsage() != availableMaxMemory  || go) {
        //report MAX Memory
        ch.setParammeter(fldMaxMEM);
        ch.setValue(availableMaxMemory);
        lastReport.setMemoryMaxUsage(availableMaxMemory);
        stChanges.push_back(ch);

    }

    if (abs(lastReport.getCompActUsage() - compActUsage) > deltaComp  || go) {
        //report CPU
        ch.setParammeter(fldActCPU);
        ch.setValue(compActUsage);
        lastReport.setCompActUsage(compActUsage);
        stChanges.push_back(ch);
    }

    if (abs(lastReport.getMemoryActUsage() - memActUsage) > deltaMemory  || go) {
        //report Memory
        ch.setParammeter(fldActMEM);
        ch.setValue(memActUsage);
        lastReport.setMemoryActUsage(memActUsage);
        stChanges.push_back(ch);

    }

    if (lastReport.getHasCamera() != hasCamera  || go) {
        //report Camera
        ch.setParammeter(fldCAM);
        ch.setValue((hasCamera ? 1 : 0));
        lastReport.setHasCamera(hasCamera);
        stChanges.push_back(ch);
    }

    if (lastReport.getHasGPU() != hasGPU  || go) {
        //report GPU
        ch.setParammeter(fldGPU);
        ch.setValue((hasGPU? 1 : 0));
        lastReport.setHasGPU(hasGPU);
        stChanges.push_back(ch);
    }

    if (lastReport.getLockedCamera() != lockedCamera  || go) {
        //report locked camera
        ch.setParammeter(fldLkCAM);
        ch.setValue((lockedCamera ? 1 : 0));
        lastReport.setLockedCamera(lockedCamera);
        stChanges.push_back(ch);
    }

    if (lastReport.getLockedFly() != lockedFly  || go) {
        //report locked fly
        ch.setParammeter(fldLkFLY);
        ch.setValue((lockedFly ? 1 : 0));
        lastReport.setLockedFly(lockedFly);
        stChanges.push_back(ch);

    }

    if (lastReport.getLockedGPU() != lockedGPU  || go) {
        //report locked GPU
        ch.setParammeter(fldLkGPU);
        ch.setValue((lockedGPU ? 1 : 0));
        lastReport.setLockedGPU(lockedGPU);
        stChanges.push_back(ch);
    }

    if (stksize < stChanges.size()) {
        lastReport.setSequenceNumber(seq);
    }

    //avoid packet oversize
    int i = 0;
    while (!stChanges.empty() && i<1000){
        Change cht = stChanges.at(0);
        //std::cout << cht.getIpAddress() << " | " << ((int) cht.getParammeter()) << " | " << cht.getValue() << endl;
        payload->appendChangesList(cht);
        stChanges.pop_front();
        i++;
    }



    payload->setChangesCount(payload->getChangesListArraySize());
    if (payload->getChangesListArraySize() > 0) {
        EV_INFO << "sending  " << payload->getChangesListArraySize() << " changes " << std::endl;
    }

    payload->addTag<CreationTimeTag>()->setCreationTime(simTime());

    //payload size
    uint32_t s = sizeof(ChangesBlock) + payload->getChangesListArraySize() * sizeof(Change);

    payload->setChunkLength(B(s));

    return payload;


}

void SimpleBroadcast1Hop::sendPacket()
{
    std::ostringstream str;
    str << packetName << "-" << numSent;
    Packet *packet = new Packet(str.str().c_str());
    if (dontFragment)
        packet->addTag<FragmentationReq>()->setDontFragment(true);

    const auto& payload = createPayload();
    packet->insertAtBack(payload);

    EV_INFO << "Packet size: " << packet->getTotalLength() << std::endl;

    L3Address destAddr = chooseDestAddr();
    emit(packetSentSignal, packet);
    socket.sendTo(packet, destAddr, destPort);
    numSent++;
}

void SimpleBroadcast1Hop::processStart()
{
    socket.setOutputGate(gate("socketOut"));
    const char *localAddress = par("localAddress");
    socket.bind(*localAddress ? L3AddressResolver().resolve(localAddress) : L3Address(), localPort);
    setSocketOptions();

    const char *destAddrs = par("destAddresses");
    cStringTokenizer tokenizer(destAddrs);
    const char *token;

    while ((token = tokenizer.nextToken()) != nullptr) {
        destAddressStr.push_back(token);
        L3Address result;
        L3AddressResolver().tryResolve(token, result);
        if (result.isUnspecified())
            EV_ERROR << "cannot resolve destination address: " << token << endl;
        destAddresses.push_back(result);
    }

    if (!destAddresses.empty()) {
        selfMsg->setKind(SEND);
        processSend();

        if (par("taskGeneration").boolValue()){
            taskMsg->setKind(NEW_T);
            clocktime_t d = par("taskCreationInterval");
            scheduleClockEventAfter(d, taskMsg);
        }
    }
    else {
        if (stopTime >= CLOCKTIME_ZERO) {
            selfMsg->setKind(STOP);
            scheduleClockEventAt(stopTime, selfMsg);
        }
    }
}

void SimpleBroadcast1Hop::processSend()
{
    sendPacket();
    clocktime_t d = par("sendInterval");
    if (stopTime < CLOCKTIME_ZERO || getClockTime() + d < stopTime) {
        selfMsg->setKind(SEND);
        scheduleClockEventAfter(d, selfMsg);
    }
    else {
        selfMsg->setKind(STOP);
        scheduleClockEventAt(stopTime, selfMsg);
    }
}

void SimpleBroadcast1Hop::processStop()
{
    socket.close();

    if (taskMsg->isScheduled()){
        cancelEvent(taskMsg);
    }
}

void SimpleBroadcast1Hop::generateNewTask()
{

}

void SimpleBroadcast1Hop::handleMessageWhenUp(cMessage *msg)
{
    if (msg->isSelfMessage()) {
        if(msg == selfMsg){
            switch (selfMsg->getKind()) {
            case START:
                processStart();
                break;

            case SEND:
                processSend();
                break;

            case STOP:
                processStop();
                break;

            default:
                throw cRuntimeError("Invalid kind %d in self message", (int)selfMsg->getKind());
            }
        }
        else if (msg == taskMsg){
            clocktime_t d = par("taskCreationInterval");

            switch (taskMsg->getKind()) {
            case NEW_T:
                generateNewTask();
                scheduleClockEventAfter(d, taskMsg);
                break;

            default:
                throw cRuntimeError("Invalid kind %d in self task message", (int)taskMsg->getKind());
            }
        }
    }
    else
        socket.processMessage(msg);

}

void SimpleBroadcast1Hop::socketDataArrived(UdpSocket *socket, Packet *packet)
{
    // process incoming packet
    processPacket(packet);
}

void SimpleBroadcast1Hop::socketErrorArrived(UdpSocket *socket, Indication *indication)
{
    EV_WARN << "Ignoring UDP error report " << indication->getName() << endl;
    delete indication;
}

void SimpleBroadcast1Hop::socketClosed(UdpSocket *socket)
{
    if (operationalState == State::STOPPING_OPERATION)
        startActiveOperationExtraTimeOrFinish(par("stopOperationExtraTime"));
}

void SimpleBroadcast1Hop::refreshDisplay() const
{
    ApplicationBase::refreshDisplay();

    char buf[100];
    sprintf(buf, "rcvd: %d pks\nsent: %d pks", numReceived, numSent);
    getDisplayString().setTagArg("t", 0, buf);
}


void SimpleBroadcast1Hop::printPacket(Packet *msg)
{

    L3Address src, dest;
    int protocol = -1;
    auto ctrl = msg->getControlInfo();
    if (ctrl != nullptr) {
        protocol = ProtocolGroup::getIpProtocolGroup()->getProtocolNumber(msg->getTag<PacketProtocolTag>()->getProtocol());
    }
    Ptr<const L3AddressTagBase> addresses = msg->findTag<L3AddressReq>();
    if (addresses == nullptr)
        addresses = msg->findTag<L3AddressInd>();
    if (addresses != nullptr) {
        src = addresses->getSrcAddress();
        dest = addresses->getDestAddress();
    }

    int srcPort = msg->getTag<L4PortInd>()->getSrcPort();

    EV_INFO << msg << endl;
    EV_INFO << "Payload length: " << msg->getByteLength() << " bytes" << endl;

    if (ctrl != nullptr)
        EV_INFO << "src: " << src << "  dest: " << dest << "  protocol=" << protocol << endl;

    for (int i = 0; i < msg->getNumTags(); i++){
        Ptr<const TagBase> t = msg->getTag(i);
        EV_INFO << "Tag " << i << " -> " << t->str() << endl;
    }
}



void SimpleBroadcast1Hop::processChangesBlock(const Ptr<const ChangesBlock>payload, L3Address srcAddr, L3Address destAddr)
{

    // std::cout << "Received ChangesBlock! Changes:" << payload->getChangesCount() <<  std::endl;
    L3Address loopbackAddress("127.0.0.1"); // Define the loopback address

    //Change *ch = new Change[payload->getChangesCount()];
    Change ch;
    for (int i=0; i<payload->getChangesCount(); i++){
        ch = payload->getChangesList(i);
//        std::cout << ch.getIpAddress() << " | " << ((int) ch.getParammeter()) << " | " << ch.getValue() << endl;

        L3Address node_addr = ch.getIpAddress();
        if ((node_addr != loopbackAddress) && (node_addr != myAddress)) {
            int tmp_num_hops = 100000;
            L3Address tmp_nextHop_address = L3Address();

            NodeData nd;
            if (nodeDataMap.count(node_addr) != 1) {
                //new node
                nd.timestamp = payload->getTimestamp();
                nd.sequenceNumber = ch.getSequenceNumber();
                nd.address = node_addr;
                nd.coord_x = 0;
                nd.coord_y = 0;
                nd.memoryActUsage = 0;
                nd.memoryMaxUsage = 0;
                nd.compActUsage = 0;
                nd.compMaxUsage = 0;
                nd.hasCamera = false;
                nd.lockedCamera = false;
                nd.hasGPU = false;
                nd.lockedGPU = false;
                nd.lockedFly = false;
//                nd.nextHop_address = payload->getIpAddress();
//                nd.num_hops = nf.getNum_hops() + 1;
                nodeDataMap[node_addr] = nd;
                for (int j=0; j<16; j++) nd.lastSeqNumber[j] = 0;
            } else {
                nd = nodeDataMap[node_addr];
            }

            //add sequence number verification
            if (nd.lastSeqNumber[ch.getParammeter()] < ch.getSequenceNumber()) {
                switch (ch.getParammeter()){
                case fldPOS_x:
                    nd.coord_x = ch.getValue();
                    break;
                case fldPOS_y:
                    nd.coord_y = ch.getValue();
                    break;
                case fldActCPU:
                    nd.compActUsage = ch.getValue();
                    break;
                case fldActMEM:
                    nd.memoryActUsage = ch.getValue();
                    break;
                case fldMaxCPU:
                    nd.compMaxUsage = ch.getValue();
                    break;
                case fldMaxMEM:
                    nd.memoryMaxUsage = ch.getValue();
                    break;
                case fldGPU:
                    nd.hasGPU = (ch.getValue() == 1);
                    break;
                case fldCAM:
                    nd.hasCamera = (ch.getValue() == 1);
                    break;
                case fldLkGPU:
                    nd.lockedGPU = (ch.getValue() == 1);
                    break;
                case fldLkCAM:
                    nd.lockedCamera = (ch.getValue() == 1);
                    break;
                case fldLkFLY:
                    nd.lockedFly = (ch.getValue() == 1);
                    break;
                default:
                    EV_ERROR << "new unidentified field " << std::endl;
                    break;
                }
                nd.lastSeqNumber[ch.getParammeter()] = ch.getSequenceNumber();
                nodeDataMap[node_addr] = nd;
                addChange(ch);
            }
        }

    }
    //EV_INFO << myAddress << " nodeDataMap size: " << nodeDataMap.size() << std::endl;

}


void SimpleBroadcast1Hop::processHeartbeat(const Ptr<const Heartbeat>payload, L3Address srcAddr, L3Address destAddr)
{

    L3Address loopbackAddress("127.0.0.1"); // Define the loopback address

    // Create or update the data associated with this IP address
    NodeData data;
    data.timestamp = simTime();
    data.sequenceNumber = payload->getSequenceNumber();
    data.address = payload->getIpAddress();
    data.coord_x = payload->getCoord_x();
    data.coord_y = payload->getCoord_y();

    data.memoryActUsage = payload->getMemoryActUsage();
    data.memoryMaxUsage = payload->getMemoryMaxUsage();
    data.compActUsage = payload->getCompActUsage();
    data.compMaxUsage = payload->getCompMaxUsage();

    data.hasCamera = payload->getHasCamera();
    data.lockedCamera = payload->getLockedCamera();

    data.hasGPU = payload->getHasGPU();
    data.lockedGPU = payload->getLockedGPU();

    data.lockedFly = payload->getLockedFly();

    data.nextHop_address = payload->getIpAddress();
    data.num_hops = 1;

    // Store or update the data in the map
    nodeDataMap[srcAddr] = data;

    size_t nodeArraySize = payload->getNodeInfoListArraySize();
    for (int i = 0; i < nodeArraySize; ++i) {
        NodeInfo nf = payload->getNodeInfoList(i);
        L3Address node_addr = nf.getIpAddress();

        if ((node_addr != loopbackAddress) && (node_addr != myAddress)) {

            int tmp_num_hops = 100000;
            L3Address tmp_nextHop_address = L3Address();
            if (nodeDataMap.count(node_addr) != 0) {
                tmp_nextHop_address = nodeDataMap[node_addr].nextHop_address;
                tmp_num_hops = nodeDataMap[node_addr].num_hops;
            }

            if (    (nodeDataMap.count(node_addr) == 0) ||
                    (nodeDataMap[node_addr].timestamp < nf.getTimestamp())
            ){
                // Create or update the data associated with this IP address
                NodeData data_nest;


                data_nest.timestamp = nf.getTimestamp();
                data_nest.sequenceNumber = nf.getSequenceNumber();
                data_nest.address = nf.getIpAddress();
                data_nest.coord_x = nf.getCoord_x();
                data_nest.coord_y = nf.getCoord_y();

                data_nest.memoryActUsage = nf.getMemoryActUsage();
                data_nest.memoryMaxUsage = nf.getMemoryMaxUsage();
                data_nest.compActUsage = nf.getCompActUsage();
                data_nest.compMaxUsage = nf.getCompMaxUsage();

                data_nest.hasCamera = nf.getHasCamera();
                data_nest.lockedCamera = nf.getLockedCamera();

                data_nest.hasGPU = nf.getHasGPU();
                data_nest.lockedGPU = nf.getLockedGPU();

                data_nest.lockedFly = nf.getLockedFly();

                data_nest.nextHop_address = payload->getIpAddress();
                data_nest.num_hops = nf.getNum_hops() + 1;

//                if (    (nodeDataMap.count(node_addr) != 0) &&
//                        (nodeDataMap[node_addr].num_hops < nf.getNum_hops())
//                ){
//                    data_nest.nextHop_address = nodeDataMap[node_addr].nextHop_address;
//                    data_nest.num_hops = nodeDataMap[node_addr].num_hops;
//                }
//                else {
//                    data_nest.nextHop_address = payload->getIpAddress();
//                    data_nest.num_hops = nf.getNum_hops() + 1;
//                }

                // Store or update the data in the map
                nodeDataMap[node_addr] = data_nest;
            }

            // Old next_hop was better
            if (    (nodeDataMap.count(node_addr) != 0) &&
                    (nodeDataMap[node_addr].num_hops > tmp_num_hops)
            ){
                nodeDataMap[node_addr].nextHop_address = tmp_nextHop_address;
                nodeDataMap[node_addr].num_hops = tmp_num_hops;
            }
        }
    }

    // Log the extracted information
    //EV_INFO << "Received Heartbeat message: seqNum=" << sequenceNumber
    //       << ", coord_x=" << coord_x << ", coord_y=" << coord_y << endl;

}

void SimpleBroadcast1Hop::addChange(Change ch){
    bool cy = true;
    for (int i=0; i<stChanges.size(); i++){
        if (stChanges[i].getIpAddress() ==ch.getIpAddress() &&
                stChanges[i].getParammeter() == ch.getParammeter()) {
            cy = false;
            if (ch.getSequenceNumber() > stChanges[i].getSequenceNumber() ) {
                //update element
                stChanges[i].setValue(ch.getValue());
                stChanges[i].setSequenceNumber(ch.getSequenceNumber());
            }
        }
    }
    if (cy) {
        //add element
        stChanges.push_back(ch);
    }
}



void SimpleBroadcast1Hop::processPacket(Packet *pk)
{
    emit(packetReceivedSignal, pk);
    EV_INFO << "Received packet: " << UdpSocket::getReceivedPacketInfo(pk) << endl;
    printPacket(pk);

    // Extract the sender's IP address
    L3Address srcAddr, destAddr;
    L3Address loopbackAddress("127.0.0.1"); // Define the loopback address

    Ptr<const L3AddressTagBase> addresses = pk->findTag<L3AddressReq>();
    if (addresses == nullptr)
        addresses = pk->findTag<L3AddressInd>();
    if (addresses != nullptr) {
        srcAddr = addresses->getSrcAddress();
        destAddr = addresses->getDestAddress();
    }

    if ((srcAddr != loopbackAddress) && (srcAddr != myAddress)) {
        // Process here
        // Check if the packet contains ChangesBlock or Heartbeat data
        if (pk->hasData<ChangesBlock>()) {
            const auto& payload = pk->peekData<ChangesBlock>();
//            std::cout << "---------" << myAddress << " Receive-----------------" <<  std::endl;
            processChangesBlock(payload, srcAddr, destAddr);



        } else
        if (pk->hasData<Heartbeat>()) {
            // Extract the Heartbeat payload
            const auto& payload = pk->peekData<Heartbeat>();

            processHeartbeat(payload, srcAddr, destAddr);
        } else {
            EV_WARN << "Received packet does not contain a Heartbeat payload." << endl;
        }
    }

    delete pk;
    numReceived++;
}

void SimpleBroadcast1Hop::handleStartOperation(LifecycleOperation *operation)
{
    clocktime_t start = std::max(startTime, getClockTime());
    if ((stopTime < CLOCKTIME_ZERO) || (start < stopTime) || (start == stopTime && startTime == stopTime)) {
        selfMsg->setKind(START);
        scheduleClockEventAt(start, selfMsg);
    }
}

void SimpleBroadcast1Hop::handleStopOperation(LifecycleOperation *operation)
{
    cancelEvent(selfMsg);
    socket.close();
    delayActiveOperationFinish(par("stopOperationTimeout"));
}

void SimpleBroadcast1Hop::handleCrashOperation(LifecycleOperation *operation)
{
    cancelClockEvent(selfMsg);
    socket.destroy(); // TODO  in real operating systems, program crash detected by OS and OS closes sockets of crashed programs.
}

} // namespace inet

