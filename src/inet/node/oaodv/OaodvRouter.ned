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

package broadcastwireless.inet.node.oaodv;

import inet.node.inet.AdhocHost;
import broadcastwireless.inet.routing.aodv.Oaodv;

//
// It models a ~WirelessHost extended with ~Aodv submodule.
//
// AODV is an on-demand MANET routing protocol. Each node
// works similar to a 'router': they manage their own routing
// tables, send request for routes and consult the IP layer via
// Netfilter hooks for data forwarding.
//
// Scenerios where AODV routing is desired use this module as a
// host module.
//
// The AODV v.2 (known as Dymo) is also implemented: ~Dymo,
// ~DymoRouter.
//
// @see ~Aodv, ~Dymo, ~DymoRouter.
//
module OaodvRouter extends AdhocHost
{
    submodules:
        oaodv: Oaodv {
            @display("p=825,226");
        }
    connections:
        oaodv.socketOut --> at.in++;
        oaodv.socketIn <-- at.out++;
}

