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
#include <queue>
#include <map>
#include <unordered_set>
#include <set>
#include <tuple>
#include <algorithm> // for std::find
#include <cmath>

#include <numeric>  // for std::accumulate

#include "inet/mobility/base/MovingMobilityBase.h"
#include "inet/applications/base/ApplicationBase.h"
#include "inet/common/clock/ClockUserModuleMixin.h"
#include "inet/transportlayer/contract/udp/UdpSocket.h"
#include "inet/networklayer/common/L3Address.h"

#include "Heartbeat_m.h"
#include "TaskREQ_m.h"

namespace inet {

extern template class ClockUserModuleMixin<ApplicationBase>;

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

        double radius; //for partial net info

        L3Address nextHop_address;
        int num_hops;

        // Add other fields as needed
    };

    struct Task_generated_extra_info
    {
        simtime_t generation_time;
        std::vector<L3Address> deployable_nodes_at_generation;
        std::vector<L3Address> decision_nodes_at_generation;
    };

    struct Task_deploy_extra_info
    {
        simtime_t deploy_time;
        double node_pos_coord_x;
        double node_pos_coord_y;
    };

    /*struct Task
    {
        L3Address gen_address;
        int id;

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
    };*/

    struct Task_Deployed_stat_info
    {
        simtime_t deploy_time;
        L3Address add_deploy_node;
        double node_pos_coord_x;
        double node_pos_coord_y;
    };
    struct Task_Generated_stat_info
    {
        simtime_t generation_time;
        L3Address gen_address;
        uint32_t id_task;
        Strategy strategy;
        std::vector<Task_Deployed_stat_info> extra_info;
    };

  protected:
    enum SelfMsgKinds { START = 1, SEND, STOP };
    enum TaskMsgKinds { NEW_T = 1 };
    enum TaskForwardMsgKinds { FORWARD = 1 };
    enum TaskAckMsgKinds { ACK_CHECK = 1 };

    enum DisseminationType { HIERARCHICAL = 1, PROGRESSIVE, PROGRESSIVE_FULL };

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
    double radius; //for partial net info


    // state
    UdpSocket socket;
    ClockEvent *selfMsg = nullptr;



    std::map<L3Address, NodeData> nodeDataMap;
    //std::map<std::pair<L3Address, uint32_t>, bool> relayMap;

    std::set<std::pair<L3Address, uint32_t>> relayedPackets;
    //std::set<std::tuple<L3Address, uint32_t, uint32_t>> relayedPackets;

    // Task Generation
    ClockEvent *taskMsg = nullptr;

    // Task Forwarding
    ClockEvent *taskForwardMsg = nullptr;
    double mean_tf = 0.001;    // 1 ms
    double stddev_tf = 0.0002;
    double maxForwardDelay = 0.001;
    struct Forwarding_Task {
        std::vector<L3Address> dests;
        TaskREQ task;
        std::vector<int> ttls;
        uint numberOfSending;
    };
    std::queue<Forwarding_Task> forwardingTask_queue;

    ClockEvent *taskAckMsg = nullptr;
    uint numberOfMaxRetry = 1;
    double ackTimer = 3.0*maxForwardDelay;
    struct Ack_Forwarding_Task {
        Forwarding_Task ft;
        std::vector<L3Address> non_ack_dests;
        std::vector<int> non_ack_ttls;
        simtime_t sendingTimestamp;
    };
    std::vector<Ack_Forwarding_Task> ackVector;

    // statistics
    int numTaskCreated = 0;
    int numSent = 0;
    int numReceived = 0;

    int reqSent = 0;

    bool ack_func = true;

    //DisseminationType dissType = HIERARCHICAL;// full table of nodes in network
    DisseminationType dissType = PROGRESSIVE;
    //DisseminationType dissType = PROGRESSIVE_FULL;


    //static simsignal_t taskDeploymentTimeSignal;   // to record times

public:

    IMobility *mob;
    L3Address myAddress;
    int myAppAddr;

    std::vector<TaskREQ> assignedTask_list; //list of assigned task
    std::map<std::pair<L3Address, uint32_t>, Task_deploy_extra_info> extra_info_deploy_tasks;

    //std::vector<std::pair<TaskREQ, simtime_t>> generatedTask_list; //list of assigned task
    std::vector<TaskREQ> generatedTask_list; //list of assigned task
    std::map<std::pair<L3Address, uint32_t>, Task_generated_extra_info> extra_info_generated_tasks;

    virtual NodeData getMyNodeData();

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
    virtual Ptr<Heartbeat> createPayload();
    virtual void processStart();
    virtual void processSend();
    virtual void processStop();

    //virtual Ptr<TaskREQmessage> createPayloadForTask(L3Address& finaldest, L3Address& nexthopdest, TaskREQ& task, int ttl);
    virtual Ptr<TaskREQmessage> createPayloadForTask(std::vector<std::tuple<L3Address, L3Address, int>>& finaldest_next_ttl, TaskREQ& task);
    virtual L3Address getBestNeighbor(TaskREQ& t);

    virtual TaskREQ parseTask();
    virtual bool isDeployFeasible(TaskREQ& task, NodeData node);
    virtual std::vector<L3Address> checkDeployDestinationAmong(TaskREQ& task, std::map<L3Address, NodeData>& nodes);
    virtual std::vector<L3Address> checkDeployDestination(TaskREQ& task);
    virtual void sendTaskTo(std::vector<L3Address>& dest, TaskREQ& task, std::vector<int>& ttl);
    virtual void deployTaskHere(TaskREQ& task);
    virtual void manageNewTask(TaskREQ& task, bool generatedHereNow);
    virtual void generateNewTask();
    virtual void processTaskREQmessage(const Ptr<const TaskREQmessage>payload, L3Address srcAddr, L3Address destAddr);
    virtual void processTaskREQ_ACKmessage(const Ptr<const TaskREQ_ACKmessage>payload, L3Address srcAddr, L3Address destAddr);

    virtual void forwardTask();
    virtual void ackTask();
    virtual void updateRadius();

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
            << ", hasGPU: " << data.hasGPU
            << ", lockedGPU: " << data.lockedGPU
            << ", hasCamera: " << data.hasCamera
            << ", lockedCamera: " << data.lockedCamera
            << ", lockedFly: " << data.lockedFly
            << " ||| nextHop_address: " << data.nextHop_address
            << ", num_hops: " << data.num_hops
            << ", radius: " << data.radius
            << " }";

    return os;
}

// Correctly overload operator<< as a non-member function
inline std::ostream& operator<<(std::ostream& os, const inet::TaskREQ& data)
{
    os << "{ GEN addr: " << data.getGen_ipAddress()
            << ", ID: " << data.getId()
            << ", Strategy: " << data.getStrategy()
            << ", DevType: " << data.getDevType()
            << ", start_timestamp: " << data.getStart_timestamp()
            << ", end_timestamp: " << data.getEnd_timestamp()
            << ", reqPosition: " << data.getReqPosition()
            << ", pos_coord_x: " << data.getPos_coord_x()
            << ", pos_coord_y: " << data.getPos_coord_y()
            << ", range: " << data.getRange()
            << ", req_lock_flyengine: " << data.getReq_lock_flyengine()
            << ", reqCamera: " << data.getReqCamera()
            << ", lockCamera: " << data.getLockCamera()
            << ", reqGPU: " << data.getReqCPU()
            << ", lockGPU: " << data.getLockGPU()
            << ", reqCPU: " << data.getReqCPU()
            << ", reqMemory: " << data.getReqMemory()
            << " }";

    return os;
}

inline std::ostream& operator<<(std::ostream& os, const std::tuple<L3Address, uint32_t, uint32_t>& data)
{
    os << "{ addr: " << std::get<0>(data)
            << ", ID task: " << std::get<1>(data)
            << ", ID send: " << std::get<2>(data)
            << " }";

    return os;
}


inline std::ostream& operator<<(std::ostream& os, const std::pair<L3Address, uint32_t>& data)
{
    os << "{ addr: " << data.first
            << ", ID task: " << data.second
            << " }";

    return os;
}

} // namespace inet


#endif /* INET_APPLICATIONS_BROADCASTWIRELESS_SIMPLEBROADCAST1HOP_H_ */
