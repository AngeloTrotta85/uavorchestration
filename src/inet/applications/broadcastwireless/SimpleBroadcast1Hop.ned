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

package broadcastwireless.inet.applications.broadcastwireless;

import inet.applications.contract.IApp;


simple SimpleBroadcast1Hop like IApp
{
    parameters:
        double computationalPower @unit(MHz) = default(100MHz); 	// computational Power
        double availableMaxMemory @unit(MB) = default (100MB);			// available space for data/task
        bool hasCamera = default(true); 
        bool hasGPU = default(true); 
        
        bool taskGeneration = default(false); 
        double taskCreationInterval @unit(s) = default(1s); 
        double taskCreationStart @unit(s) = default(10s); 
        
        double startMakingStats @unit(s) = default(0s);
        
        int dissType = default(1); //HIERARCHICAL = 1, PROGRESSIVE = 2, HIERARCHICAL_CHANGES = 3
        int strategyType = default(1); //STRATEGY_FORALL = 1, STRATEGY_EXISTS = 2
        double gamma_almost_all = default(2);
        double gamma_at_least_one = default(1.7);
        
         
        string interfaceTableModule;   // The path to the InterfaceTable module
        string clockModule = default(""); // relative path of a module that implements IClock; optional
        int localPort = default(-1);  // local port (-1: use ephemeral port)
        string destAddresses = default(""); // list of IP addresses, separated by spaces ("": don't send)
        string localAddress = default("");
        string packetName = default("UdpBasicAppData");
        int destPort;
        volatile int messageLength @unit(B); // length of messages to generate, in bytes
        double startTime @unit(s) = default(this.sendInterval); // application start time (start of the first packet)
        double stopTime @unit(s) = default(-1s);  // time of finishing sending, -1s means forever
        volatile double sendInterval @unit(s); // should usually be a random value, e.g. exponential(1)
        int timeToLive = default(-1); // if not -1, set the TTL (IPv4) or Hop Limit (IPv6) field of sent packets to this value
        bool dontFragment = default(false); // if true, asks IP to not fragment the message during routing
        int dscp = default(-1); // if not -1, set the DSCP field (on IPv4/IPv6) of sent packets to this value
        int tos = default(-1); // if not -1, set the Type Of Service (IPv4) / Traffic Class (IPv6) field of sent packets to this value
        string multicastInterface = default("");  // if not empty, set the multicast output interface option on the socket (interface name expected)
        bool receiveBroadcast = default(false); // if true, makes the socket receive broadcast packets
        bool joinLocalMulticastGroups = default(false); // if true, makes the socket receive packets from all multicast groups set on local interfaces
        @display("i=block/app");
        @class(::inet::SimpleBroadcast1Hop);
        @lifecycleSupport;
        double stopOperationExtraTime @unit(s) = default(-1s);    // extra time after lifecycle stop operation finished
        double stopOperationTimeout @unit(s) = default(2s);    // timeout value for lifecycle stop operation
        @signal[packetSent](type=inet::Packet);
        @signal[packetReceived](type=inet::Packet);
        
        
        @statistic[packetReceived](title="packets received"; source=packetReceived; record=count,"sum(packetBytes)","vector(packetBytes)"; interpolationmode=none);
        @statistic[throughput](title="throughput"; unit=bps; source="throughput(packetReceived)"; record=vector);
        @statistic[packetSent](title="packets sent"; source=packetSent; record=count,"sum(packetBytes)","vector(packetBytes)"; interpolationmode=none);
        @statistic[rcvdPkLifetime](title="received packet lifetime"; source="dataAge(packetReceived)"; unit=s; record=stats,vector; interpolationmode=none);
        @statistic[rcvdPkSeqNo](title="received packet sequence number"; source="appPkSeqNo(packetReceived)"; record=vector; interpolationmode=none);
        

    gates:
        input socketIn @labels(UdpControlInfo/up);
        output socketOut @labels(UdpControlInfo/down);
}

