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

//Node kept data:
//
//	NodeData:
//	- Address
//	- CPU
//	- Mem
//	- CAM
//	- GPU
//	- Coord
//	- radius
//	onehopneighbors:
//		- Address
//		- CPU (best member)
//		- Mem (maximum)
//		- CAM (available in the area)
//		- GPU (available in the area)
//		- Coord
//		- radius
//		
//		...
//		- Address
//		- CPU (best member)
//		- Mem (maximum)
//		- CAM (available in the area)
//		- GPU (available in the area)
//		- Coord
//		- radius
//		
//
//Broadcast Data:
//	- Address
//	- CPU (best member)
//	- Mem (maximum)
//	- CAM (available in the area)
//	- GPU (available in the area)
//	- Coord
//	- radius - I sent 0 when I still don't have neighbors
// 
//
//
//Process broadcasted data:
//- If I receive a broadcasted packet, it is my on-hop neighbor
//- Update my radius by the distance to the faraway neighbor.



import inet.common.INETDefs;
import inet.common.packet.chunk.Chunk;
import inet.networklayer.common.L3Address;

namespace inet;

class SendBstProt
{
    simtime_t timestamp;
    uint32_t sequenceNumber;
    L3Address ipAddress;
    double coord_x;
    double coord_y;
    double radius;
    
    double memoryActUsage;
    double memoryMaxUsage;    
    double compActUsage;
    double compMaxUsage;
    bool hasCamera;
    bool lockedCamera;
    bool hasGPU;
    bool lockedGPU;
    bool lockedFly;
}












