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


import inet.common.INETDefs;
import inet.common.packet.chunk.Chunk;

import inet.networklayer.common.L3Address;

namespace inet;

enum Strategy
{
    STRATEGY_FORALL = 0;
    STRATEGY_MANY = 1;
    STRATEGY_EXISTS = 2;
    STRATEGY_EXAC = 3;
};

enum DevType
{
    DEVTYPE_DRONE = 0;
    DEVTYPE_CLOUD = 1;
};

enum DeployType 
{ 
    HIERARCHICAL_MSG = 1;
    PROGRESSIVE_MSG = 2;
};



class TaskREQ
{
    L3Address gen_ipAddress;
    uint32_t id;
    simtime_t gen_timestamp;
    
    Strategy strategy; 
    DevType devType; 
    
    simtime_t start_timestamp;
    simtime_t end_timestamp;
    
    //Position
    bool reqPosition;
    double pos_coord_x;
    double pos_coord_y;
    double range;

    bool req_lock_flyengine;
    
    //Hardware
    
    //Camera
    bool reqCamera;
    bool lockCamera;
    
    //GPU
    bool reqGPU;
    bool lockGPU;
    
    //CPU
    double reqCPU;
    
    //Memory
    double reqMemory;
    
}

class DestDetail 
{
    int ttl;
    L3Address dest_ipAddress;
    L3Address nextHop_ipAddress;
}

class TaskREQmessage extends FieldsChunk
{
    int idReqMessage;
    //int ttl;
    //L3Address dest_ipAddress;
    //L3Address nextHop_ipAddress;
    DestDetail destDetail[];
    
    DeployType depStrategy;
    
    TaskREQ task;
}


class TaskREQ_ACKmessage extends FieldsChunk
{
    L3Address dest_ipAddress;
    L3Address src_ipAddress;
    
    
    TaskREQ task;
}



