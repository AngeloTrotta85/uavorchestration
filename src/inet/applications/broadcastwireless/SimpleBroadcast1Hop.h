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

#ifndef INET_APPLICATIONS_BROADCASTWIRELESS_SIMPLEBROADCAST1HOP_H_
#define INET_APPLICATIONS_BROADCASTWIRELESS_SIMPLEBROADCAST1HOP_H_

#include <vector>
#include <map>
#include <cmath>
#include <deque>

#include "inet/mobility/base/MovingMobilityBase.h"
#include "inet/applications/base/ApplicationBase.h"
#include "inet/common/clock/ClockUserModuleMixin.h"
#include "inet/transportlayer/contract/udp/UdpSocket.h"
#include "inet/networklayer/common/L3Address.h"

#include "Heartbeat_m.h"

namespace inet {

extern template class ClockUserModuleMixin<ApplicationBase>;

enum idField : uint8_t { //Field Id
    fldActCPU,
    fldMaxCPU,
    fldActMEM,
    fldMaxMEM,
    fldPOS_x,
    fldPOS_y,
    fldGPU,
    fldCAM,
    fldLkCAM,
    fldLkFLY,
    fldLkGPU
};


/**
 * UDP application. See NED for more info.
 */
class INET_API SimpleBroadcast1Hop : public ClockUserModuleMixin<ApplicationBase>, public UdpSocket::ICallback
{
public:
    struct NodeData
    {
        simtime_t timestamp;
        int sequenceNumber;
        L3Address address;
        double coord_x;
        double coord_y;
        double memoryActUsage;
        double memoryMaxUsage;
        double compActUsage;
        double compMaxUsage;

        bool hasCamera;
        bool lockedCamera;

        bool hasGPU;
        bool lockedGPU;

        bool lockedFly;

        L3Address nextHop_address;
        int num_hops;
        // Add other fields as needed
        uint32_t lastSeqNumber[16]; //last received sequence number for each field
    };

    struct Task
    {
        simtime_t start_timestamp;
        simtime_t end_timestamp;
        double req_computation;
        double req_memory;

        bool req_camera;
        bool req_GPU;
        bool req_flyengine;

        bool req_lock_camera;
        bool req_lock_GPU;
        bool req_lock_flyengine;
    };


protected:
    enum SelfMsgKinds { START = 1, SEND, STOP };
    enum TaskMsgKinds { NEW_T = 1 };

    // parameters
    std::vector<L3Address> destAddresses;
    std::vector<std::string> destAddressStr;
    int localPort = -1, destPort = -1;
    clocktime_t startTime;
    clocktime_t stopTime;
    bool dontFragment = false;
    const char *packetName = nullptr;
    //
    double computationalPower;    // computational Power: [operation/sec]
    double availableMaxMemory;
    bool hasCamera;
    bool hasGPU;

    NodeInfo lastReport; // last report data
    std::deque<Change> stChanges;

    double compActUsage = 0;
    double memActUsage = 0;
    bool lockedGPU = false;
    bool lockedCamera = false;
    bool lockedFly = false;


    // state
    UdpSocket socket;
    ClockEvent *selfMsg = nullptr;
    //
    std::vector<struct Task> assignedTask_list; //list of assigned task
    //double availableActualMemory;

    IMobility *mob;
    L3Address myAddress;

    std::map<L3Address, NodeData> nodeDataMap;

    // Task Generation
    ClockEvent *taskMsg = nullptr;

    // statistics
    int numSent = 0;
    int numReceived = 0;

  protected:
    virtual int numInitStages() const override { return NUM_INIT_STAGES; }
    virtual void initialize(int stage) override;
    virtual void handleMessageWhenUp(cMessage *msg) override;
    virtual void finish() override;
    virtual void refreshDisplay() const override;

    // chooses random destination address
    virtual L3Address chooseDestAddr();
    virtual void sendPacket();
    virtual void printPacket(Packet *msg);
    virtual void processPacket(Packet *msg);
    virtual void setSocketOptions();

    virtual void processHeartbeat(const Ptr<const Heartbeat> payload, L3Address srcAddr, L3Address destAddr);
    virtual void processChangesBlock(const Ptr<const ChangesBlock> payload, L3Address srcAddr, L3Address destAddr);
    virtual void addChange(Change ch);
    virtual void updateTasks();
    virtual void updateChanges();
    virtual Ptr<ChangesBlock> createPayload();
    virtual void processStart();
    virtual void processSend();
    virtual void processStop();

    virtual void generateNewTask();

    virtual void handleStartOperation(LifecycleOperation *operation) override;
    virtual void handleStopOperation(LifecycleOperation *operation) override;
    virtual void handleCrashOperation(LifecycleOperation *operation) override;

    virtual void socketDataArrived(UdpSocket *socket, Packet *packet) override;
    virtual void socketErrorArrived(UdpSocket *socket, Indication *indication) override;
    virtual void socketClosed(UdpSocket *socket) override;

  public:
    SimpleBroadcast1Hop() {}
    ~SimpleBroadcast1Hop();
};

// Correctly overload operator<< as a non-member function

inline std::ostream& operator<<(std::ostream& os, const SimpleBroadcast1Hop::NodeData& data)
{
    os << "{ sequenceNumber: " << data.sequenceNumber
            << ", address: " << data.address
            << ", timestamp: " << data.timestamp
            << ", coord_x: " << data.coord_x
            << ", coord_y: " << data.coord_y
            << ", memoryActUsage: " << data.memoryActUsage
            << ", memoryMaxUsage: " << data.memoryMaxUsage
            << ", compActUsage: " << data.compActUsage
            << ", compMaxUsage: " << data.compMaxUsage
            << " ||| nextHop_address: " << data.nextHop_address
            << ", num_hops: " << data.num_hops
            << ", lastSeqNumber: ";
    for (int i=0; i<16; i++) os << data.lastSeqNumber[i] << ",";
    os << " }";

    return os;
}

} // namespace inet


#endif /* INET_APPLICATIONS_BROADCASTWIRELESS_SIMPLEBROADCAST1HOP_H_ */
