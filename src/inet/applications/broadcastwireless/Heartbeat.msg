//
// Copyright (C) 2016 OpenSim Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later
//

import inet.common.INETDefs;
import inet.common.packet.chunk.Chunk;

import inet.networklayer.common.L3Address;

namespace inet;

class NodeInfo
{
    simtime_t timestamp;
    uint32_t sequenceNumber;
    L3Address ipAddress;
    
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
}

//
// Generic application packet
//
// TODO rename to ApplicationHeader and remove ByteCountChunk from ApplicationHeaderSerializer
class Heartbeat extends FieldsChunk
{
    uint32_t sequenceNumber;
    
    L3Address ipAddress;
    
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
    
    NodeInfo nodeInfoList[];
    
}
