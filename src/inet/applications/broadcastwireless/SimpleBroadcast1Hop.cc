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

Ptr<Heartbeat> SimpleBroadcast1Hop::createPayload()
{
    const auto& payload = makeShared<Heartbeat>();

    payload->setChunkLength(B(par("messageLength")));

    payload->setSequenceNumber(numSent);
    payload->setIpAddress(myAddress);
    payload->setCoord_x(mob->getCurrentPosition().x);
    payload->setCoord_y(mob->getCurrentPosition().y);

    payload->setCompMaxUsage(computationalPower);
    payload->setMemoryMaxUsage(availableMaxMemory);
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
    payload->setCompActUsage(compActUsage);
    payload->setMemoryActUsage(memActUsage);

    payload->setHasCamera(hasCamera);
    payload->setHasGPU(hasGPU);

    payload->setLockedCamera(lockedCamera);
    payload->setLockedFly(lockedFly);
    payload->setLockedGPU(lockedGPU);

    int i = 0;
    payload->setNodeInfoListArraySize(nodeDataMap.size());
    for (std::map<inet::L3Address, NodeData>::iterator it = nodeDataMap.begin(); it != nodeDataMap.end(); ++it) {
        L3Address ipAddr = it->first;
        NodeData data = it->second;

        NodeInfo new_NodeInfo;
        new_NodeInfo.setTimestamp(data.timestamp);
        new_NodeInfo.setSequenceNumber(data.sequenceNumber);
        new_NodeInfo.setIpAddress(data.address);
        new_NodeInfo.setCoord_x(data.coord_x);
        new_NodeInfo.setCoord_y(data.coord_y);
        new_NodeInfo.setMemoryActUsage(data.memoryActUsage);
        new_NodeInfo.setMemoryMaxUsage(data.memoryMaxUsage);
        new_NodeInfo.setCompActUsage(data.compActUsage);
        new_NodeInfo.setCompMaxUsage(data.compMaxUsage);

        new_NodeInfo.setNextHop_address(data.nextHop_address);
        new_NodeInfo.setNum_hops(data.num_hops);

        new_NodeInfo.setHasGPU(data.hasGPU);
        new_NodeInfo.setHasCamera(data.hasCamera);

        new_NodeInfo.setLockedCamera(data.lockedCamera);
        new_NodeInfo.setLockedGPU(data.lockedGPU);
        new_NodeInfo.setLockedFly(data.lockedFly);

        payload->setNodeInfoList(i, new_NodeInfo);

        i++;
    }

    payload->addTag<CreationTimeTag>()->setCreationTime(simTime());

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

//    const auto& payload = makeShared<Heartbeat>();
//    payload->setChunkLength(B(par("messageLength")));
//
//    payload->setSequenceNumber(numSent);
//    payload->setIpAddress(myAddress);
//    payload->setCoord_x(mob->getCurrentPosition().x);
//    payload->setCoord_y(mob->getCurrentPosition().y);
//
//    payload->setCompMaxUsage(computationalPower);
//    payload->setMemoryMaxUsage(availableMaxMemory);
//    double compActUsage = 0;
//    double memActUsage = 0;
//    for (size_t i = 0; i < assignedTask_list.size(); ++i) {
//        if ((simTime() < assignedTask_list[i].end_timestamp) && (simTime() >= assignedTask_list[i].start_timestamp)){
//            compActUsage += assignedTask_list[i].req_computation;
//            memActUsage += assignedTask_list[i].req_memory;
//        }
//    }
//    payload->setCompActUsage(compActUsage);
//    payload->setMemoryActUsage(memActUsage);
//
//    int i = 0;
//    payload->setNodeInfoListArraySize(nodeDataMap.size());
//    for (std::map<inet::L3Address, NodeData>::iterator it = nodeDataMap.begin(); it != nodeDataMap.end(); ++it) {
//        L3Address ipAddr = it->first;
//        NodeData data = it->second;
//
//        NodeInfo new_NodeInfo;
//        new_NodeInfo.setTimestamp(data.timestamp);
//        new_NodeInfo.setSequenceNumber(data.sequenceNumber);
//        new_NodeInfo.setIpAddress(data.address);
//        new_NodeInfo.setCoord_x(data.coord_x);
//        new_NodeInfo.setCoord_y(data.coord_y);
//        new_NodeInfo.setMemoryActUsage(data.memoryActUsage);
//        new_NodeInfo.setMemoryMaxUsage(data.memoryMaxUsage);
//        new_NodeInfo.setCompActUsage(data.compActUsage);
//        new_NodeInfo.setCompMaxUsage(data.compMaxUsage);
//
//        new_NodeInfo.setNextHop_address(data.nextHop_address);
//        new_NodeInfo.setNum_hops(data.num_hops);
//
//        payload->setNodeInfoList(i, new_NodeInfo);
//
//        i++;
//    }
//
//    payload->addTag<CreationTimeTag>()->setCreationTime(simTime());
    packet->insertAtBack(payload);
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
        // Check if the packet contains Heartbeat data
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

