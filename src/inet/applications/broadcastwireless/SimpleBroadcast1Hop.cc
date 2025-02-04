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


static bool gEnableDebug = false;
static inline void debugPrint(const char* fmt, ...)
{
    if (!gEnableDebug)
        return;

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

//...
//
//// Then use:
//debugPrint("SimpleBroadcast1Hop::processTaskREQ_ACKmessage::begin\n");


namespace inet {

Define_Module(SimpleBroadcast1Hop);

//simsignal_t SimpleBroadcast1Hop::taskDeploymentTimeSignal = registerSignal("tDeploymentTimeSignal");

SimpleBroadcast1Hop::~SimpleBroadcast1Hop()
{
    cancelAndDelete(selfMsg);
    cancelAndDelete(taskMsg);
    cancelAndDelete(taskForwardMsg);
    cancelAndDelete(taskAckMsg);
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
        WATCH_VECTOR(assignedTask_list);
        WATCH_SET(relayedPackets);

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
        taskForwardMsg = new ClockEvent("taskForwardTimer");
        taskAckMsg = new ClockEvent("TaskAckTimer");


    }
    else if (stage == INITSTAGE_LAST){
        mob = check_and_cast<IMobility *>(this->getParentModule()->getSubmodule("mobility"));

        EV_INFO << "Node position is: " << mob->getCurrentPosition() << endl;

        // Obtain the IP address
        myAddress = L3AddressResolver().addressOf(getParentModule(), L3AddressResolver::ADDR_IPv4);

        myAppAddr = this->getParentModule()->getIndex();

        // Log or store the IP address
        EV_INFO << "My IP address is: " << myAddress.str() << endl;
        EV_INFO << "My APP address is: " << myAppAddr << endl;

    }
}

void SimpleBroadcast1Hop::finish()
{
    recordScalar("packets sent", numSent);
    recordScalar("packets received", numReceived);

    if (myAppAddr == 0) {
        int nnodes = this->getParentModule()->getVectorSize();
        double total_task_deployed = 0;
        double total_task_generated = 0;

        std::vector<TaskREQ> full_generatedTask_list;
        std::map<std::pair<L3Address, uint32_t>, Task_generated_extra_info> full_generatedTask_info_list;

        std::map<std::pair<L3Address, uint32_t>, int> n_deply_per_task;
        std::map<std::pair<L3Address, uint32_t>, std::vector<double>> time_n_deply_per_task;
        std::map<std::pair<L3Address, uint32_t>, std::vector<L3Address>> location_deply_per_task;

        std::map<std::pair<L3Address, uint32_t>, Task_Generated_stat_info> extra_info_per_task;

        for (int n = 0; n < nnodes; ++n) {
            SimpleBroadcast1Hop *appn = check_and_cast<SimpleBroadcast1Hop *>(this->getParentModule()->getParentModule()->getSubmodule("host", n)->getSubmodule("app", 0));
            L3Address n_ipaddr = appn->myAddress;

            EV_INFO << "Host " << n << " has IP address: " << n_ipaddr.str() << endl;

            full_generatedTask_list.insert(full_generatedTask_list.end(), appn->generatedTask_list.begin(), appn->generatedTask_list.end());
            //full_generatedTask_info_list.insert(full_generatedTask_info_list.end(), appn->extra_info_generated_tasks.begin(), appn->extra_info_generated_tasks.end());
            for (const auto & kv : appn->extra_info_generated_tasks) {
                // Overwrite duplicates from appn->extra_info_generated_tasks:
                full_generatedTask_info_list[kv.first] = kv.second;
            }

            for (auto& t : appn->generatedTask_list) {
                std::pair<L3Address, uint32_t> key_map = std::make_pair(t.getGen_ipAddress(), t.getId());
                n_deply_per_task[key_map] = 0;
                time_n_deply_per_task[key_map] = std::vector<double>();
                location_deply_per_task[key_map] = std::vector<L3Address>();
                extra_info_per_task[key_map].generation_time = t.getGen_timestamp();
                extra_info_per_task[key_map].gen_address = t.getGen_ipAddress();
                extra_info_per_task[key_map].id_task = t.getId();
                extra_info_per_task[key_map].strategy = t.getStrategy();
            }

            total_task_deployed += appn->assignedTask_list.size();
            total_task_generated += appn->generatedTask_list.size();

        }

        for (int n = 0; n < nnodes; ++n) {
            SimpleBroadcast1Hop *appn = check_and_cast<SimpleBroadcast1Hop *>(this->getParentModule()->getParentModule()->getSubmodule("host", n)->getSubmodule("app", 0));
            L3Address n_ipaddr = appn->myAddress;

            for (auto& t_deploy : appn->assignedTask_list) {
                std::pair<L3Address, uint32_t> key_map = std::make_pair(t_deploy.getGen_ipAddress(), t_deploy.getId());

                n_deply_per_task[key_map] += 1;


                double time_to_deploy = (appn->extra_info_deploy_tasks[key_map].deploy_time - full_generatedTask_info_list[key_map].generation_time).dbl();
                time_n_deply_per_task[key_map].push_back(time_to_deploy);
                location_deply_per_task[key_map].push_back(n_ipaddr);

                Task_Deployed_stat_info si;
                si.add_deploy_node = n_ipaddr;
                si.deploy_time = time_to_deploy;
                si.node_pos_coord_x = appn->extra_info_deploy_tasks[key_map].node_pos_coord_x;
                si.node_pos_coord_y = appn->extra_info_deploy_tasks[key_map].node_pos_coord_y;
                extra_info_per_task[key_map].extra_info.push_back(si);
            }
        }


        std::vector<L3Address> onlyExpected_all;
        std::vector<L3Address> onlyActual_all;
        std::vector<L3Address> both_all;
        std::vector<double> onlyExpected_all_size;
        std::vector<double> onlyActual_all_size;
        std::vector<double> both_all_size;

        //printf("Test1"); fflush(stdout);

        for (int n = 0; n < nnodes; ++n) {
            SimpleBroadcast1Hop *appn = check_and_cast<SimpleBroadcast1Hop *>(this->getParentModule()->getParentModule()->getSubmodule("host", n)->getSubmodule("app", 0));
            L3Address n_ipaddr = appn->myAddress;

            //printf("Test1_1"); fflush(stdout);

            for (auto& gt : appn->generatedTask_list) {
                std::pair<L3Address, uint32_t> key_map = std::make_pair(gt.getGen_ipAddress(), gt.getId());

                std::vector<L3Address> expected_deployments = appn->extra_info_generated_tasks[key_map].deployable_nodes_at_generation;

                //printf("Test1_1_1"); fflush(stdout);

                std::vector<L3Address> actual_deployments;
                for (int n2 = 0; n2 < nnodes; ++n2) {
                    SimpleBroadcast1Hop *appn2 = check_and_cast<SimpleBroadcast1Hop *>(this->getParentModule()->getParentModule()->getSubmodule("host", n2)->getSubmodule("app", 0));
                    L3Address n_ipaddr2 = appn2->myAddress;

                    if (appn2->extra_info_deploy_tasks.count(key_map) != 0) {
                        actual_deployments.push_back(n_ipaddr2);
                    }
                }

                //printf("Test1_1_2"); fflush(stdout);

                if (actual_deployments.size() > 0) {

                    std::vector<L3Address> onlyExpected;
                    std::vector<L3Address> onlyActual;
                    std::vector<L3Address> both;

                    for (const auto &e : expected_deployments) {
                        // Check if e is in actual_deployments
                        bool found = (std::find(actual_deployments.begin(), actual_deployments.end(), e) != actual_deployments.end());
                        if (found) {
                            both.push_back(e);
                        } else {
                            onlyExpected.push_back(e);
                        }
                    }

                    //printf("Test1_1_3"); fflush(stdout);

                    for (const auto &a : actual_deployments) {
                        // Check if a is in expected_deployments
                        bool found = (std::find(expected_deployments.begin(), expected_deployments.end(), a)
                                != expected_deployments.end());
                        if (!found) {
                            onlyActual.push_back(a);
                        }
                    }

                    //printf("Test1_1_4"); fflush(stdout);

                    onlyExpected_all.insert(onlyExpected_all.end(), onlyExpected.begin(), onlyExpected.end());
                    onlyActual_all.insert(onlyActual_all.end(), onlyActual.begin(), onlyActual.end());
                    both_all.insert(both_all.end(), both.begin(), both.end());

                    //printf("Test1_1_5"); fflush(stdout);

                    if ((onlyExpected.size() + both.size()) > 0) {
                        double sss = onlyExpected.size() + both.size();
                        double oe = onlyExpected.size() / sss;;
                        double oa = onlyActual.size() / sss;
                        double b = both.size() / sss;

                        printf("TASK:%s-%d\n", key_map.first.str().c_str(), key_map.second);
                        printf("OE:"); for (auto& el : onlyExpected) printf("%s - ", el.str().c_str()); printf("\n");
                        printf("OA:"); for (auto& el : onlyActual) printf("%s - ", el.str().c_str()); printf("\n");
                        printf("B:"); for (auto& el : both) printf("%s - ", el.str().c_str()); printf("\n");
                        printf("onlyExpected: %f \n", oe);
                        printf("onlyActual: %f \n", oa);
                        printf("both: %f \n\n", b);

                        onlyExpected_all_size.push_back(oe);
                        onlyActual_all_size.push_back(oa);
                        both_all_size.push_back(b);

//                        onlyExpected_all_size.push_back(onlyExpected.size());
//                        onlyActual_all_size.push_back(onlyActual.size());
//                        both_all_size.push_back(both.size());
                    }

                    //printf("Test1_1_6"); fflush(stdout);
                }

            }

            //printf("Test1_2"); fflush(stdout);
        }

        //printf("Test2"); fflush(stdout);


        recordScalar("expected only deployment total", onlyExpected_all.size());
        recordScalar("actual only deployment total", onlyActual_all.size());
        recordScalar("expected and actual deployment total", both_all.size());


        double expected_size = 0;
        double actual_size = 0;
        double both_size = 0;

        if (onlyExpected_all_size.size() > 0) {
            double sum = std::accumulate(onlyExpected_all_size.begin(), onlyExpected_all_size.end(), 0.0);
            expected_size = sum / onlyExpected_all_size.size();
        }
        if (onlyActual_all_size.size() > 0) {
            double sum = std::accumulate(onlyActual_all_size.begin(), onlyActual_all_size.end(), 0.0);
            actual_size = sum / onlyActual_all_size.size();
        }
        if (both_all_size.size() > 0) {
            double sum = std::accumulate(both_all_size.begin(), both_all_size.end(), 0.0);
            both_size = sum / both_all_size.size();
        }

        recordScalar("expected only deployment size total", expected_size);
        recordScalar("actual only deployment size total", actual_size);
        recordScalar("expected and actual deployment size total", both_size);




        int n_deployed_at_least_1 = 0;
        for (const auto & kv : n_deply_per_task) {
            if (kv.second > 1) n_deployed_at_least_1 += 1;
        }

        double ratio_task_deployed_at_least_1_total = 1;
        if (total_task_generated > 0) ratio_task_deployed_at_least_1_total = n_deployed_at_least_1/total_task_generated;

        double ratio_task_deployed_total = 1;
        if (total_task_generated > 0) ratio_task_deployed_total = total_task_deployed/total_task_generated;

        recordScalar("task generated total", total_task_generated);
        recordScalar("task deployed total", total_task_deployed);
        recordScalar("ratio task deployed total", ratio_task_deployed_total);
        recordScalar("ratio task deployed at_least_1 total", ratio_task_deployed_at_least_1_total);

        double avg_delay = 0;
        std::vector<double> vall;
        for (auto& el : time_n_deply_per_task){
            vall.insert(vall.end(), el.second.begin(), el.second.end());
        }
        if (vall.size() > 0) {
            double sum = std::accumulate(vall.begin(), vall.end(), 0.0);
            avg_delay = sum / vall.size();
        }
        recordScalar("task deployed time avg", avg_delay);


        for (auto& tg : full_generatedTask_list) {
            std::pair<L3Address, uint32_t> key_map = std::make_pair(tg.getGen_ipAddress(), tg.getId());

            printf("TASK:%s-%d DEPLOYED IN:\n", key_map.first.str().c_str(), key_map.second);
            for (auto& dd : location_deply_per_task[key_map]) {
                printf("node:%s - ", dd.str().c_str());
            }
            printf("\n");
        }


        //#########################################################################################
        //#########################################################################################
        //#########################################################################################

        // remake of stats
        printf("\n\nGOOD STATS\n");
        double ok_total_task_generated = full_generatedTask_list.size();
        double ok_total_task_deployed = 0;
        double ok_task_deployed_atleast1_if_deployable = 0;
        double ok_task_deployed_atleast1 = 0;
        double ok_task_deployed_sum_if_deployable = 0;
        double ok_total_task_generated_deployable = 0;

        double ok_task_deployed_latency_sum = 0;

        std::vector<double> ok_onlyExpected_all_size;
        std::vector<double> ok_onlyActual_all_size;
        std::vector<double> ok_both_all_size;
        std::vector<double> ok_onlyExpectedDecision_all_size;
        std::vector<double> ok_onlyActualDecision_all_size;
        std::vector<double> ok_bothDecision_all_size;

        for (auto& tg : full_generatedTask_list) {
            std::pair<L3Address, uint32_t> key_map = std::make_pair(tg.getGen_ipAddress(), tg.getId());
            Task_Generated_stat_info act_stat = extra_info_per_task[key_map];
            Task_generated_extra_info task_stat = full_generatedTask_info_list[key_map];








            printf("  TASK:%s-%d, strategy %d. Generated at time %s.\n",
                    key_map.first.str().c_str(), key_map.second, act_stat.strategy, act_stat.generation_time.str().c_str());

            std::vector<L3Address> expected_deployments = task_stat.deployable_nodes_at_generation;
            printf("  Deployable nodes: ");
            for (auto& dn : task_stat.deployable_nodes_at_generation){
                printf("%s ", dn.str().c_str());
            }
            printf("\n");

            std::vector<L3Address> decision_deployments = task_stat.decision_nodes_at_generation;
            printf("  Decision where to deploy nodes: ");
            for (auto& dn : task_stat.decision_nodes_at_generation){
                printf("%s ", dn.str().c_str());
            }
            printf("\n");

            std::vector<L3Address> actual_deployments;
            printf("  Deployed in:\n");
            for (auto& dd : act_stat.extra_info) {
                ok_total_task_deployed++;
                ok_task_deployed_latency_sum += dd.deploy_time.dbl();
                actual_deployments.push_back(dd.add_deploy_node);

                printf("    node:%s; deploy time:%s \n",
                        dd.add_deploy_node.str().c_str(), dd.deploy_time.str().c_str());
            }
            //printf("\n");


            if (task_stat.deployable_nodes_at_generation.size() > 0) {
                ok_total_task_generated_deployable++;

                if (act_stat.extra_info.size() > 0) {
                    ok_task_deployed_atleast1_if_deployable++;
                }
                ok_task_deployed_sum_if_deployable += act_stat.extra_info.size();
            }

            if (act_stat.extra_info.size() > 0) {
                ok_task_deployed_atleast1++;
            }


            if (actual_deployments.size() > 0) {

                std::vector<L3Address> onlyExpected;
                std::vector<L3Address> onlyActual;
                std::vector<L3Address> both;

                for (const auto &e : expected_deployments) {
                    // Check if e is in actual_deployments
                    bool found = (std::find(actual_deployments.begin(), actual_deployments.end(), e) != actual_deployments.end());
                    if (found) {
                        both.push_back(e);
                    } else {
                        onlyExpected.push_back(e);
                    }
                }

                for (const auto &a : actual_deployments) {
                    // Check if a is in expected_deployments
                    bool found = (std::find(expected_deployments.begin(), expected_deployments.end(), a)
                            != expected_deployments.end());
                    if (!found) {
                        onlyActual.push_back(a);
                    }
                }


                if ((onlyExpected.size() + both.size()) > 0) {
                    double sss = onlyExpected.size() + both.size();
                    double oe = onlyExpected.size() / sss;;
                    double oa = onlyActual.size() / sss;
                    double b = both.size() / sss;

                    printf("      TASK:%s-%d\n", key_map.first.str().c_str(), key_map.second);
                    printf("      OE:"); for (auto& el : onlyExpected) printf("%s - ", el.str().c_str()); printf("\n");
                    printf("      OA:"); for (auto& el : onlyActual) printf("%s - ", el.str().c_str()); printf("\n");
                    printf("      B:"); for (auto& el : both) printf("%s - ", el.str().c_str()); printf("\n");
                    printf("      onlyExpected: %f \n", oe);
                    printf("      onlyActual: %f \n", oa);
                    printf("      both: %f \n\n", b);

                    ok_onlyExpected_all_size.push_back(oe);
                    ok_onlyActual_all_size.push_back(oa);
                    ok_both_all_size.push_back(b);

                }
            }

            if (actual_deployments.size() > 0) {

                std::vector<L3Address> onlyExpected;
                std::vector<L3Address> onlyActual;
                std::vector<L3Address> both;

                for (const auto &e : decision_deployments) {
                    // Check if e is in actual_deployments
                    bool found = (std::find(actual_deployments.begin(), actual_deployments.end(), e) != actual_deployments.end());
                    if (found) {
                        both.push_back(e);
                    } else {
                        onlyExpected.push_back(e);
                    }
                }

                for (const auto &a : actual_deployments) {
                    // Check if a is in expected_deployments
                    bool found = (std::find(decision_deployments.begin(), decision_deployments.end(), a)
                            != decision_deployments.end());
                    if (!found) {
                        onlyActual.push_back(a);
                    }
                }


                if ((onlyExpected.size() + both.size()) > 0) {
                    double sss = onlyExpected.size() + both.size();
                    double oe = onlyExpected.size() / sss;;
                    double oa = onlyActual.size() / sss;
                    double b = both.size() / sss;

                    printf("      TASK:%s-%d\n", key_map.first.str().c_str(), key_map.second);
                    printf("      Decision OE:"); for (auto& el : onlyExpected) printf("%s - ", el.str().c_str()); printf("\n");
                    printf("      Decision OA:"); for (auto& el : onlyActual) printf("%s - ", el.str().c_str()); printf("\n");
                    printf("      Decision B:"); for (auto& el : both) printf("%s - ", el.str().c_str()); printf("\n");
                    printf("      Decision onlyExpected: %f \n", oe);
                    printf("      Decision onlyActual: %f \n", oa);
                    printf("      Decision both: %f \n\n", b);

                    ok_onlyExpectedDecision_all_size.push_back(oe);
                    ok_onlyActualDecision_all_size.push_back(oa);
                    ok_bothDecision_all_size.push_back(b);

                }
            }


            printf("\n");
        }

        double ok_avg_delay = 0;
        if (ok_total_task_deployed > 0) {
            ok_avg_delay = ok_task_deployed_latency_sum / ok_total_task_deployed;
        }

        double ok_deployed_deployable_atleast1_ratio = 0;
        if (ok_total_task_generated_deployable > 0) {
            ok_deployed_deployable_atleast1_ratio = ok_task_deployed_atleast1_if_deployable / ok_total_task_generated_deployable;
        }

        double ok_task_deployed_sum_if_deployable_avg = 0;
        if (ok_total_task_generated_deployable > 0) {
            ok_total_task_generated_deployable = ok_task_deployed_sum_if_deployable / ok_total_task_generated_deployable;
        }

        double ok_expected_size = 0;
        double ok_actual_size = 0;
        double ok_both_size = 0;
        if (ok_onlyExpected_all_size.size() > 0) {
            double sum = std::accumulate(ok_onlyExpected_all_size.begin(), ok_onlyExpected_all_size.end(), 0.0);
            ok_expected_size = sum / ok_onlyExpected_all_size.size();
        }
        if (ok_onlyActual_all_size.size() > 0) {
            double sum = std::accumulate(ok_onlyActual_all_size.begin(), ok_onlyActual_all_size.end(), 0.0);
            ok_actual_size = sum / ok_onlyActual_all_size.size();
        }
        if (ok_both_all_size.size() > 0) {
            double sum = std::accumulate(ok_both_all_size.begin(), ok_both_all_size.end(), 0.0);
            ok_both_size = sum / ok_both_all_size.size();
        }




        double ok_expectedDecision_size = 0;
        double ok_actualDecision_size = 0;
        double ok_bothDecision_size = 0;
        if (ok_onlyExpectedDecision_all_size.size() > 0) {
            double sum = std::accumulate(ok_onlyExpectedDecision_all_size.begin(), ok_onlyExpectedDecision_all_size.end(), 0.0);
            ok_expectedDecision_size = sum / ok_onlyExpectedDecision_all_size.size();
        }
        if (ok_onlyActualDecision_all_size.size() > 0) {
            double sum = std::accumulate(ok_onlyActualDecision_all_size.begin(), ok_onlyActualDecision_all_size.end(), 0.0);
            ok_actualDecision_size = sum / ok_onlyActualDecision_all_size.size();
        }
        if (ok_bothDecision_all_size.size() > 0) {
            double sum = std::accumulate(ok_bothDecision_all_size.begin(), ok_bothDecision_all_size.end(), 0.0);
            ok_bothDecision_size = sum / ok_bothDecision_all_size.size();
        }

        // #################

        recordScalar("OK - task deployed time avg", ok_avg_delay);
        recordScalar("OK - task generated number", ok_total_task_generated);
        recordScalar("OK - task deployable generated number", ok_total_task_generated_deployable);
        recordScalar("OK - task total deployment number", ok_total_task_deployed);
        recordScalar("OK - task total deployment at least 1 number", ok_task_deployed_atleast1);
        recordScalar("OK - task deployable total deployment at least 1 number", ok_task_deployed_atleast1_if_deployable);
        recordScalar("OK - task deployable total deployment at least 1 number - Ratio", ok_deployed_deployable_atleast1_ratio);
        recordScalar("OK - task deployable total deployment number for each task - AVG", ok_total_task_generated_deployable);

        recordScalar("OK - expected only deployment size total", ok_expected_size);
        recordScalar("OK - actual only deployment size total", ok_actual_size);
        recordScalar("OK - expected and actual deployment size total", ok_both_size);

        recordScalar("OK - expected Decision only deployment size total", ok_expectedDecision_size);
        recordScalar("OK - actual Decision only deployment size total", ok_actualDecision_size);
        recordScalar("OK - expected and actual Decision deployment size total", ok_bothDecision_size);
    }

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

void SimpleBroadcast1Hop::updateRadius(){
    double x1 = mob->getCurrentPosition().x;
    double y1 = mob->getCurrentPosition().y;
    radius = 0;
    for (std::map<inet::L3Address, NodeData>::iterator it = nodeDataMap.begin(); it != nodeDataMap.end(); ++it) {
        L3Address ipAddr = it->first;
        NodeData data = it->second;
        //calculate new radius - if granter than previous, overwrite it
        double r = sqrt((x1-data.coord_x)*(x1-data.coord_x) + (y1-data.coord_y)*(y1-data.coord_y));
        if (r > radius) radius = r;
    }
}

Ptr<Heartbeat> SimpleBroadcast1Hop::createPayload()
{
    const auto& payload = makeShared<Heartbeat>();

    payload->setChunkLength(B(par("messageLength")));

    payload->setSequenceNumber(numSent);
    payload->setIpAddress(myAddress);
    payload->setCoord_x(mob->getCurrentPosition().x);
    payload->setCoord_y(mob->getCurrentPosition().y);


    //update self use by assigned tasks
    double compActUsage = 0;
    double memActUsage = 0;
    bool lockedGPU = false;
    bool lockedCamera = false;
    bool lockedFly = false;
    for (size_t i = 0; i < assignedTask_list.size(); ++i) {
        if ((simTime() < assignedTask_list[i].getEnd_timestamp()) && (simTime() >= assignedTask_list[i].getStart_timestamp())){
            compActUsage += assignedTask_list[i].getReqCPU();
            memActUsage += assignedTask_list[i].getReqMemory();
            if (assignedTask_list[i].getLockGPU()) lockedGPU = true;
            if (assignedTask_list[i].getLockCamera()) lockedCamera = true;
            if (assignedTask_list[i].getReq_lock_flyengine()) lockedFly = true;
        }
    }

    payload->setCompMaxUsage(computationalPower);
    payload->setMemoryMaxUsage(availableMaxMemory);

    payload->setCompActUsage(compActUsage);
    payload->setMemoryActUsage(memActUsage);

    payload->setHasCamera(hasCamera);
    payload->setHasGPU(hasGPU);

    payload->setLockedCamera(lockedCamera);
    payload->setLockedFly(lockedFly);
    payload->setLockedGPU(lockedGPU);


    if (dissType == PROGRESSIVE) {
        //in Aggregated knowledge, I will send only 1-hop nodes
        //choose the best metrics between me and my 1-hop neighbors and broadcast it
        updateRadius();
        payload->setRadius(radius);

        for (std::map<inet::L3Address, NodeData>::iterator it = nodeDataMap.begin(); it != nodeDataMap.end(); ++it) {
            L3Address ipAddr = it->first;
            NodeData data = it->second;

            // I need to overwrite the metrics based on best choices

            //rule for CompMaxUsage
            if (data.compMaxUsage > payload->getCompMaxUsage())
                payload->setCompMaxUsage(data.memoryMaxUsage);

            //rule for MemoryMaxUsage
            if (data.memoryMaxUsage > payload->getMemoryMaxUsage())
                payload->setMemoryMaxUsage(data.memoryMaxUsage);

            //rule for CompActUsage - choose the minor
            if (data.compActUsage < payload->getCompActUsage())
                payload->setCompActUsage(data.compActUsage);

            //rule for MemoryActUsage - choose the minor
            if (data.memoryActUsage < payload->getMemoryActUsage())
                payload->setMemoryActUsage(data.memoryActUsage);

            //rule for HasCamera and LockedCamera
            payload->setHasCamera(payload->getHasCamera() || data.hasCamera);
            payload->setLockedCamera(!payload->getHasCamera()&&data.lockedCamera ||
                                    payload->getLockedCamera()&&!data.hasCamera ||
                                    payload->getLockedCamera()&&data.lockedCamera);


            //rule for HasGPU and LockedGPU
            payload->setHasGPU(payload->getHasGPU() || data.hasGPU);
            payload->setLockedGPU(!payload->getHasGPU()&&data.lockedGPU ||
                                    payload->getLockedGPU()&&!data.hasGPU ||
                                    payload->getLockedGPU()&&data.lockedGPU);

            //rule for LockedFly
            payload->setLockedFly((lockedFly && data.lockedFly));
        }


    } else {
        //sending full node table data
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
            new_NodeInfo.setNextHop_address(data.nextHop_address);
            new_NodeInfo.setNum_hops(data.num_hops);

            new_NodeInfo.setHasGPU(data.hasGPU);
            new_NodeInfo.setHasCamera(data.hasCamera);
            new_NodeInfo.setHasGPU(data.hasGPU);
            new_NodeInfo.setHasCamera(data.hasCamera);

            new_NodeInfo.setLockedCamera(data.lockedCamera);
            new_NodeInfo.setLockedGPU(data.lockedGPU);
            new_NodeInfo.setLockedFly(data.lockedFly);
            new_NodeInfo.setLockedCamera(data.lockedCamera);
            new_NodeInfo.setLockedGPU(data.lockedGPU);
            new_NodeInfo.setLockedFly(data.lockedFly);

            payload->setNodeInfoList(i, new_NodeInfo);
            payload->setNodeInfoList(i, new_NodeInfo);

            i++;
        }
    }
    payload->addTag<CreationTimeTag>()->setCreationTime(simTime());

    return payload;
}

void SimpleBroadcast1Hop::sendPacket()
{
    std::ostringstream str;

    if (dissType == HIERARCHICAL_CHANGES) {
        str << "Changes" << "-" << numSent; //
        Packet *packet = new Packet(str.str().c_str());
        if (dontFragment)
            packet->addTag<FragmentationReq>()->setDontFragment(true);
        const auto& payload = createChangesPayload();
        packet->insertAtBack(payload);
        L3Address destAddr = chooseDestAddr();
        emit(packetSentSignal, packet);
        socket.sendTo(packet, destAddr, destPort);

    } else {
        str << packetName << "-" << numSent; //
        Packet *packet = new Packet(str.str().c_str());
        if (dontFragment)
            packet->addTag<FragmentationReq>()->setDontFragment(true);
        const auto& payload = createPayload();
        packet->insertAtBack(payload);
        L3Address destAddr = chooseDestAddr();
        emit(packetSentSignal, packet);
        socket.sendTo(packet, destAddr, destPort);

    }
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
            scheduleClockEventAfter(d*3, taskMsg);
        }
        taskForwardMsg->setKind(FORWARD);
        //scheduleClockEventAfter(truncnormal(mean_tf, stddev_tf), taskForwardMsg);


        if (ack_func){
            taskAckMsg->setKind(ACK_CHECK);
            scheduleClockEventAfter(truncnormal(1, 0.01), taskAckMsg);
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

void SimpleBroadcast1Hop::ackTask()
{
    debugPrint("SimpleBroadcast1Hop::ackTask::begin\n");
    simtime_t nowT = simTime();

    for (auto it = ackVector.begin(); it != ackVector.end(); ) {
        if ((nowT - it->sendingTimestamp).dbl() > ackTimer)
        {
            if (forwardingTask_queue.empty()) {
                scheduleClockEventAfter(uniform(0, maxForwardDelay), taskForwardMsg);
            }

            // Enqueue the task to re-send it
            Forwarding_Task ft;
            ft.dests = it->non_ack_dests;
            ft.task = it->ft.task;
            ft.ttls = it->non_ack_ttls;
            ft.numberOfSending = 0;
            forwardingTask_queue.push(ft);

            // Remove the element from ackVector; erase returns an iterator
            // pointing to the next element after the erased one
            it = ackVector.erase(it);

            EV_INFO << "ACK expired sending TASK again" << endl;
        }
        else
        {
            // Only advance if we haven't erased
            ++it;
        }
    }
    debugPrint("SimpleBroadcast1Hop::ackTask::end\n");
}

void SimpleBroadcast1Hop::forwardTask()
{
    // Dequeue
    if (!forwardingTask_queue.empty()) {
        // Access the front task
        Forwarding_Task& frontTask = forwardingTask_queue.front();

        // Forward to all destinations
        sendTaskTo(frontTask.dests, frontTask.task, frontTask.ttls);

        if (ack_func) {
            if (frontTask.numberOfSending < numberOfMaxRetry) {

                //adding it to the ack list
                Ack_Forwarding_Task newAFT;
                newAFT.ft = frontTask;
                newAFT.ft.numberOfSending += 1;
                newAFT.non_ack_dests.insert(newAFT.non_ack_dests.begin(), frontTask.dests.begin(), frontTask.dests.end());
                newAFT.non_ack_ttls.insert(newAFT.non_ack_ttls.begin(), frontTask.ttls.begin(), frontTask.ttls.end());
                newAFT.sendingTimestamp = simTime();
                ackVector.push_back(newAFT);
            }
        }

        // Once handled, pop it
        forwardingTask_queue.pop();
        EV_INFO << "Forwarded one task. Queue size now: " << forwardingTask_queue.size() << endl;
    }
}

TaskREQ SimpleBroadcast1Hop::parseTask() // TODO
{
    TaskREQ newTask = TaskREQ();

    //EXAMPLE OF SCANPEOPLE
    newTask.setStrategy(STRATEGY_FORALL);
    //newTask.setStrategy(STRATEGY_MANY);
    //newTask.setStrategy(STRATEGY_EXISTS);
    //newTask.setStrategy(STRATEGY_EXAC);

    newTask.setDevType(DEVTYPE_DRONE);
    newTask.setReqPosition(true);
    newTask.setPos_coord_x(uniform(630, 1900));//(1560);
    newTask.setPos_coord_y(uniform(630, 1900)); //(1560);
    newTask.setRange(630);
    newTask.setReqCamera(true);
    newTask.setReqCPU(3);
    newTask.setReqMemory(2);
    //newTask.setReq_lock_flyengine(true);

    newTask.setStart_timestamp(simTime());
    newTask.setEnd_timestamp(simTime()+1000000);

    newTask.setId(numTaskCreated);
    numTaskCreated++;
    newTask.setGen_ipAddress(myAddress);
    newTask.setGen_timestamp(simTime());

    return newTask;
}

static bool isInsideCircle(double xCenter, double yCenter, double radius,
                    double xPoint, double yPoint)
{
    double dx = xPoint - xCenter;
    double dy = yPoint - yCenter;
    double distSquared = dx * dx + dy * dy;
    double radiusSquared = radius * radius;
    return (distSquared <= radiusSquared);
}


bool SimpleBroadcast1Hop::isDeployFeasible(TaskREQ& task, NodeData node) {
    bool ris = true;

    //check coords
    if (task.getReqPosition() && (!isInsideCircle(task.getPos_coord_x(), task.getPos_coord_y(), task.getRange(), node.coord_x, node.coord_y)))
        ris = false;

    //check GPU
    if ((task.getReqCPU() && node.lockedGPU) || (task.getReqCPU() && !node.hasGPU))
        ris = false;

    //check locked Fly
    if (task.getReq_lock_flyengine() && node.lockedFly)
        return false;

    //check camera
    if ((task.getReqCamera() && node.lockedCamera) || (task.getReqCamera() && !node.hasCamera))
        ris = false;

    //check CPU
    if ((task.getReqCPU() + node.compActUsage) > node.compMaxUsage)
        ris = false;


    //check Memory
    if ((task.getReqMemory() + node.memoryActUsage) > node.memoryMaxUsage)
        ris = false;

    return ris;
}

static double directionFactor(double xa, double ya,
                 double xb, double yb,
                 double xd, double yd,
                 double alphaDegrees)
{
    // 1) Build vectors AD and AB
    double ADx = xd - xa;
    double ADy = yd - ya;
    double ABx = xb - xa;
    double ABy = yb - ya;

    // 2) Compute dot product
    double dot = (ADx * ABx) + (ADy * ABy);

    // 3) Compute magnitudes (watch out for zero length)
    double magAD = std::sqrt(ADx*ADx + ADy*ADy);
    double magAB = std::sqrt(ABx*ABx + ABy*ABy);

    // If either vector is zero-length, angle is undefined; handle as needed
    if (magAD < 1e-9 || magAB < 1e-9) {
        return false;
    }

    // 4) Compute angle in radians
    double cosTheta = dot / (magAD * magAB);

    // Numerical safety: clamp cosTheta to [-1, 1] to avoid domain errors in acos
    if (cosTheta > 1.0)  cosTheta = 1.0;
    else if (cosTheta < -1.0) cosTheta = -1.0;

    double thetaRad = std::acos(cosTheta);
    // Convert to degrees
    double thetaDeg = thetaRad * 180.0 / M_PI;

    // 5) Compare to alpha
    //return (thetaDeg <= alphaDegrees);

    // Now compute piecewise-linear factor:
    // f(theta) = 1 - (theta / alpha), clipped to [0, 1].
    if (thetaDeg <= 0.0) {
        return 1.0; // the vectors are basically identical direction
    } else if (thetaDeg >= alphaDegrees) {
        return 0.0; // angle is outside arc
    } else {
        return 1.0 - (thetaDeg / alphaDegrees);
    }
}
//
//int main()
//{
//    double xa = 0, ya = 0;
//    double xb = 1, yb = 1;
//    double xd = 2, yd = 0;
//    double alpha = 45.0;  // degrees
//
//    bool result = isWithinArc(xa, ya, xb, yb, xd, yd, alpha);
//    std::cout << "B is "
//              << (result ? "" : "not ")
//              << "within " << alpha << " degrees of A->D\n";
//    return 0;
//}


double SimpleBroadcast1Hop::calculateProgressiveScore(TaskREQ& task, NodeData node) {
    double ris = 0;
    double pos_fact = 1;
    double gpu_fact = 1;
    double mem_fact = 1;
    double cpu_fact = 1;
    double fly_fact = 1;
    double cam_fact = 1;

    //check coords
    if (task.getReqPosition()){
        pos_fact = directionFactor(mob->getCurrentPosition().x, mob->getCurrentPosition().y, node.coord_x, node.coord_y, task.getPos_coord_x(), task.getPos_coord_y(), 90);
    }

    //check GPU
    if (task.getReqCPU()) {
        if (node.lockedGPU || !node.hasGPU) {
            gpu_fact = 0;
        }
    }

    //check locked Fly
    if (task.getReq_lock_flyengine() && node.lockedFly)
        fly_fact = 0;

    //check camera
    if ((task.getReqCamera() && node.lockedCamera) || (task.getReqCamera() && !node.hasCamera))
        cam_fact = 0;

    //check CPU

    if ((task.getReqCPU() + node.compActUsage) > node.compMaxUsage)
        cpu_fact = node.compMaxUsage / (task.getReqCPU() + node.compActUsage);


    //check Memory
    if ((task.getReqMemory() + node.memoryActUsage) > node.memoryMaxUsage)
        mem_fact = node.memoryMaxUsage / (task.getReqMemory() + node.memoryActUsage);

    return (0.5 * pos_fact) + (0.5 * ((gpu_fact + fly_fact + cam_fact + cpu_fact + mem_fact) / 5.0));
}

std::vector<L3Address> SimpleBroadcast1Hop::checkDeployDestinationAmong_Progressive(TaskREQ& task, std::map<L3Address, NodeData>& nodes)
{
    std::vector<L3Address> ris;
    std::map<L3Address, double> nodeDataMap_score;

    for (std::map<inet::L3Address, NodeData>::iterator it = nodes.begin(); it != nodes.end(); ++it) {
        double score = calculateProgressiveScore(task, it->second);
        if (score > 0.5)
            nodeDataMap_score[it->first] = score;
    }

    EV_INFO << "checkDeployDestinationAmong_Progressive - Calculated SCORES: " << endl;
    for (std::map<inet::L3Address, double>::iterator it = nodeDataMap_score.begin(); it != nodeDataMap_score.end(); ++it) {
        EV_INFO << it->first << " with score " << it->second << endl;
    }

    size_t mapSize = nodeDataMap_score.size();

    if (mapSize != 0){
        if ((task.getStrategy() == STRATEGY_FORALL) || (task.getStrategy() == STRATEGY_MANY)) {
            for (std::map<inet::L3Address, double>::iterator it = nodeDataMap_score.begin(); it != nodeDataMap_score.end(); ++it) {
                ris.push_back(it->first);
            }
        }
        else {
            L3Address best = L3Address("0.0.0.0");
            double score_max = 0;
            for (std::map<inet::L3Address, double>::iterator it = nodeDataMap_score.begin(); it != nodeDataMap_score.end(); ++it) {
                if (it->second > score_max) {
                    best = it->first;
                    score_max = it->second;
                }
            }
            ris.push_back(best);

        }

    }

    return ris;
}

std::vector<L3Address> SimpleBroadcast1Hop::checkDeployDestinationAmong(TaskREQ& task, std::map<L3Address, NodeData>& nodes)
{
    std::vector<L3Address> ris;
    if (dissType == PROGRESSIVE) return ris;

    std::map<L3Address, NodeData> nodeDataMap_feasible;
    //for (std::map<inet::L3Address, NodeData>::iterator it = nodeDataMap.begin(); it != nodeDataMap.end(); ++it) {
    for (std::map<inet::L3Address, NodeData>::iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (isDeployFeasible(task, it->second))
            nodeDataMap_feasible[it->first] = it->second;
    }

    size_t mapSize = nodeDataMap_feasible.size();

//    if (mapSize == 0){
//        return Ipv4Address::UNSPECIFIED_ADDRESS;
//    }
//    else {
//
//        //choose randomly
//        int randomIndex = intuniform(0, mapSize - 1);
//        auto it = nodeDataMap_feasible.begin();
//        std::advance(it, randomIndex);
//        EV_INFO << "Deploying TASK to: " << it->first << endl;
//        return it->first;
//    }

    if (mapSize != 0){
        if ((task.getStrategy() == STRATEGY_FORALL) || (task.getStrategy() == STRATEGY_MANY)) {
            for (std::map<inet::L3Address, NodeData>::iterator it = nodeDataMap_feasible.begin(); it != nodeDataMap_feasible.end(); ++it) {
                ris.push_back(it->first);
            }
        }
        else {
            //choose randomly
            int randomIndex = intuniform(0, mapSize - 1);
            auto it = nodeDataMap_feasible.begin();
            std::advance(it, randomIndex);
            EV_INFO << "Deploying TASK to: " << it->first << endl;
            //return it->first;
            ris.push_back(it->first);
        }

    }

    return ris;
}

SimpleBroadcast1Hop::NodeData SimpleBroadcast1Hop::getMyNodeData(){
    NodeData mydata;

    double compActUsage = 0;
    double memActUsage = 0;
    bool lockedGPU = false;
    bool lockedCamera = false;
    bool lockedFly = false;
    for (size_t i = 0; i < assignedTask_list.size(); ++i) {
        if ((simTime() < assignedTask_list[i].getEnd_timestamp()) && (simTime() >= assignedTask_list[i].getStart_timestamp())){
            compActUsage += assignedTask_list[i].getReqCPU();
            memActUsage += assignedTask_list[i].getReqMemory();
            if (assignedTask_list[i].getLockGPU()) lockedGPU = true;
            if (assignedTask_list[i].getLockCamera()) lockedCamera = true;
            if (assignedTask_list[i].getReq_lock_flyengine()) lockedFly = true;
        }
    }

    mydata.timestamp = simTime();
    mydata.sequenceNumber = numSent;
    mydata.address = myAddress;
    mydata.coord_x = mob->getCurrentPosition().x;
    mydata.coord_y = mob->getCurrentPosition().y;
    mydata.memoryActUsage = memActUsage;
    mydata.memoryMaxUsage = availableMaxMemory;
    mydata.compActUsage = compActUsage;
    mydata.compMaxUsage = computationalPower;
    mydata.hasCamera = hasCamera;
    mydata.lockedCamera = lockedCamera;
    mydata.hasGPU = hasGPU;
    mydata.lockedGPU = lockedGPU;
    mydata.lockedFly = lockedFly;
    mydata.nextHop_address = myAddress;
    mydata.num_hops = 0;

    return mydata;
}

std::vector<L3Address> SimpleBroadcast1Hop::checkDeployDestination(TaskREQ& task, L3Address avoidAddress)
{

    std::map<L3Address, NodeData> nodeDataMap_all;
    NodeData mydata = getMyNodeData();

//    double compActUsage = 0;
//    double memActUsage = 0;
//    bool lockedGPU = false;
//    bool lockedCamera = false;
//    bool lockedFly = false;
//    for (size_t i = 0; i < assignedTask_list.size(); ++i) {
//        if ((simTime() < assignedTask_list[i].getEnd_timestamp()) && (simTime() >= assignedTask_list[i].getStart_timestamp())){
//            compActUsage += assignedTask_list[i].getReqCPU();
//            memActUsage += assignedTask_list[i].getReqMemory();
//            if (assignedTask_list[i].getLockGPU()) lockedGPU = true;
//            if (assignedTask_list[i].getLockCamera()) lockedCamera = true;
//            if (assignedTask_list[i].getReq_lock_flyengine()) lockedFly = true;
//        }
//    }
//
//    mydata.timestamp = simTime();
//    mydata.sequenceNumber = numSent;
//    mydata.address = myAddress;
//    mydata.coord_x = mob->getCurrentPosition().x;
//    mydata.coord_y = mob->getCurrentPosition().y;
//    mydata.memoryActUsage = memActUsage;
//    mydata.memoryMaxUsage = availableMaxMemory;
//    mydata.compActUsage = compActUsage;
//    mydata.compMaxUsage = computationalPower;
//    mydata.hasCamera = hasCamera;
//    mydata.lockedCamera = lockedCamera;
//    mydata.hasGPU = hasGPU;
//    mydata.lockedGPU = lockedGPU;
//    mydata.lockedFly = lockedFly;
//    mydata.nextHop_address = myAddress;
//    mydata.num_hops = 0;

    nodeDataMap_all[myAddress] = mydata;
    for (std::map<inet::L3Address, NodeData>::iterator it = nodeDataMap.begin(); it != nodeDataMap.end(); ++it) {
        if (it->first != avoidAddress)
            nodeDataMap_all[it->first] = it->second;
    }

    EV_INFO << "SimpleBroadcast1Hop::checkDeployDestination. ALL DEVICES" << endl;
    for (auto& el : nodeDataMap_all) {
        EV_INFO << el.first << " | " << el.second << endl;
    }

    if (dissType == HIERARCHICAL)
        return checkDeployDestinationAmong(task, nodeDataMap_all);
    else
        return checkDeployDestinationAmong_Progressive(task, nodeDataMap_all);

//    size_t mapSize = nodeDataMap.size();
//    double r = uniform(0, 1);
//    if ((r < 0.2) || (mapSize == 0)) {
//        L3Address loopbackAddress("127.0.0.1");
//        EV_INFO << "Deploying TASK locally: " << loopbackAddress << endl;
//        return loopbackAddress;
//    }
//    else {
//        int randomIndex = intuniform(0, mapSize - 1);
//        auto it = nodeDataMap.begin();
//        std::advance(it, randomIndex);
//        EV_INFO << "Deploying TASK to: " << it->first << endl;
//        return it->first;
//    }

}


Ptr<TaskREQmessage> SimpleBroadcast1Hop::createPayloadForTask(std::vector<std::tuple<L3Address, L3Address, int>>& finaldest_next_ttl, TaskREQ& task)
{
    const auto& payload = makeShared<TaskREQmessage>();

    payload->setChunkLength(B(par("messageLength"))); // TODO

    payload->setIdReqMessage(reqSent);
    if (dissType == HIERARCHICAL) {
        payload->setDepStrategy(HIERARCHICAL_MSG);
    }
    else if (dissType == PROGRESSIVE) {
        payload->setDepStrategy(PROGRESSIVE_MSG);
    }
    else {
        payload->setDepStrategy(HIERARCHICAL_MSG); //TODO
    }


    payload->setDestDetailArraySize(finaldest_next_ttl.size());
    for (int i = 0; i < finaldest_next_ttl.size(); ++i) {
        DestDetail destDetail;
        destDetail.setDest_ipAddress(std::get<0>(finaldest_next_ttl[i]));
        destDetail.setNextHop_ipAddress(std::get<1>(finaldest_next_ttl[i]));
        destDetail.setTtl(std::get<2>(finaldest_next_ttl[i]));

        payload->setDestDetail(i, destDetail);
    }

//    payload->setDest_ipAddress(finaldest);
//    payload->setNextHop_ipAddress(nexthopdest);
//    payload->setTtl(ttl);  // TODO

    payload->setTask(task);

    return payload;
}

void SimpleBroadcast1Hop::sendTaskTo(std::vector<L3Address>& dest, TaskREQ& task, std::vector<int>& ttl)
{
    std::vector<std::tuple<L3Address, L3Address, int>> dest_next_ttl;

    int i = 0;
    for (auto& d : dest){
        if (nodeDataMap.count(d) != 0) {
            NodeData data = nodeDataMap[d];
            auto tupleValue = std::make_tuple(d, data.nextHop_address, ttl[i]); // @suppress("Function cannot be instantiated")
            dest_next_ttl.push_back(tupleValue);

            //dest_next_ttl.push_back(std::make_tuple(d, data.nextHop_address, ttl[i]));
        }

        i++;
    }

    if (dest_next_ttl.size() > 0) {
        std::ostringstream str;
        str << "Task-" << task.getGen_ipAddress().str() << "-" << task.getId() << "-" << reqSent;
        Packet *packet = new Packet(str.str().c_str());
        if (dontFragment)
            packet->addTag<FragmentationReq>()->setDontFragment(true);

        //const auto& payload = createPayloadForTask(dest, destAddr,  task, ttl);
        const auto& payload = createPayloadForTask(dest_next_ttl, task);

        packet->insertAtBack(payload);

        for (auto& dd : dest_next_ttl)
            EV_INFO << "Sending TASK to: " << std::get<0>(dd) << " passing from " << std::get<1>(dd) << " with ttl " << std::get<2>(dd)<< endl;

        EV_INFO << "SENDING Packet name: " << packet->getName() << ", length: " << packet->getTotalLength() << "\n";
        EV_INFO << "SENDING Tags:\n";
        for (int i = 0; i < packet->getNumTags(); i++) {
            auto tag = packet->getTag(i);
            EV_INFO << "  Tag " << i << ": " << tag->str() << "\n";
        }

        if (dissType != PROGRESSIVE) {
            //broadcast
            socket.sendTo(packet, L3Address("255.255.255.255"), destPort);
        } else {
            socket.sendTo(packet, dest[0], destPort);
        }

        //reqSent++;
    }
    else {
        EV_WARN << "SimpleBroadcast1Hop::sendTaskTo NO DESTINATION FOUND FOR THE TASK" << endl;
    }


//    if (nodeDataMap.count(dest) != 0) {
//        NodeData data = nodeDataMap[dest];
//
//        std::ostringstream str;
//        str << "Task-" << task.getGen_ipAddress().str() << "-" << task.getId() << "-" << reqSent;
//        Packet *packet = new Packet(str.str().c_str());
//        if (dontFragment)
//            packet->addTag<FragmentationReq>()->setDontFragment(true);
//
//
//        L3Address destAddr = data.nextHop_address;
//
//        const auto& payload = createPayloadForTask(dest, destAddr,  task, ttl);
//
//        packet->insertAtBack(payload);
//
//        EV_INFO << "Sending TASK to: " << dest << " passing from " << destAddr << endl;
//
//        //emit(packetSentSignal, packet);
//        socket.sendTo(packet, L3Address("255.255.255.255"), destPort);
//
//        reqSent++;
//    }
//    else {
//        EV_WARN << "SimpleBroadcast1Hop::sendTaskTo NO DESTINATION FOUND FOR THE TASK" << endl;
//    }
}

bool SimpleBroadcast1Hop::isDeployFeasibleLocal(TaskREQ& task) {
    NodeData nd = getMyNodeData();
    return isDeployFeasible(task, nd);
}

void SimpleBroadcast1Hop::deployTaskHere(TaskREQ& task)
{
    //Check if already deployed
    for (auto& at : assignedTask_list){
        if ((at.getGen_ipAddress() == task.getGen_ipAddress()) && (at.getId() == task.getId())) {
            return;
        }
    }

    // Check if I have the capabilities
    if (!isDeployFeasibleLocal(task)) {
        EV_INFO << myAddress << " NOT deploying TASK here, I don't meet the constraints" << endl;
        return;
    }

    EV_INFO << "Deploying TASK NOW: " << task << endl;
    assignedTask_list.push_back(task);

    Task_deploy_extra_info extra;
    extra.deploy_time = simTime();
    extra.node_pos_coord_x = mob->getCurrentPosition().x;
    extra.node_pos_coord_y = mob->getCurrentPosition().y;
    extra_info_deploy_tasks[std::make_pair(task.getGen_ipAddress(), task.getId())] = extra;

    // If not already deployed, record the deployment time
    simtime_t generationTime = task.getGen_timestamp();
    simtime_t deployTime = simTime();
    simtime_t timeToDeploy = deployTime - generationTime;

    // Emit the deployment time
    //emit(taskDeploymentTimeSignal, timeToDeploy.dbl());  // record as double (in seconds)

}

void SimpleBroadcast1Hop::manageNewTask(TaskREQ& task, bool generatedHereNow, L3Address avoidAddress)
{
    L3Address loopbackAddress("127.0.0.1");
    std::vector<L3Address> deployDest_out;
    std::vector<int> ttls;
    std::vector<L3Address> deployDest = checkDeployDestination(task);

    if (dissType != PROGRESSIVE) {
        if (generatedHereNow) {
            Task_generated_extra_info extra;
            extra.generation_time = simTime();

            int nnodes = this->getParentModule()->getVectorSize();
            for (int n = 0; n < nnodes; ++n) {
                SimpleBroadcast1Hop *appn = check_and_cast<SimpleBroadcast1Hop *>(this->getParentModule()->getParentModule()->getSubmodule("host", n)->getSubmodule("app", 0));
                L3Address n_ipaddr = appn->myAddress;
                NodeData n_data = appn->getMyNodeData();

                if (isDeployFeasible(task, n_data) ) {
                    extra.deployable_nodes_at_generation.push_back(n_ipaddr);
                }
            }

            extra.decision_nodes_at_generation.insert(extra.decision_nodes_at_generation.end(), deployDest.begin(), deployDest.end());

            extra_info_generated_tasks[std::make_pair(task.getGen_ipAddress(), task.getId())] = extra;
        }
    } else {
        L3Address finalDest = getBestNeighbor(task);
        if (finalDest == myAddress) {
            NodeData node = getMyNodeData();
            if (isDeployFeasible(task, node)){
                deployTaskHere(task);
            }
        } else {
            deployDest_out.push_back(finalDest);
            ttls.push_back(10);
        }
    }
    //if (deployDest == Ipv4Address::UNSPECIFIED_ADDRESS) {
    if (deployDest.size() == 0) {
        EV_INFO << "NO place where to deploy TASK!" << endl;
    }
    else {
        EV_INFO << "Sending TASK to these destinations:" << endl;
        for (auto& dest : deployDest) {
            EV_INFO << "Dest:" << dest << endl;
            if ((dest == loopbackAddress) || (dest == myAddress)) {
                deployTaskHere(task);
            }
            else {
                deployDest_out.push_back(dest);
                ttls.push_back(10);
                //sendTaskTo(dest, task, 10); // TODO
            }
        }
        if (deployDest_out.size() > 0) {
            //sendTaskTo(deployDest_out, task, ttls);

            if (forwardingTask_queue.empty()) {
                scheduleClockEventAfter(uniform(0, maxForwardDelay), taskForwardMsg);
            }

            /*
            for (int i = 0; i < deployDest_out.size(); ++i) {
                // Enqueue
                Forwarding_Task ft;
                ft.dests.push_back(deployDest_out[i]);
                ft.task = task;
                ft.ttls.push_back(ttls[i]);
                forwardingTask_queue.push(ft);
            }
            */

            // Enqueue
            Forwarding_Task ft;
            ft.dests = deployDest_out;
            ft.task = task;
            ft.ttls = ttls;
            ft.numberOfSending = 0;
            forwardingTask_queue.push(ft);


            //reqSent++;
        }
    }
}

void SimpleBroadcast1Hop::processTaskREQ_ACKmessage(const Ptr<const TaskREQ_ACKmessage>payload, L3Address srcAddr, L3Address destAddr)
{
    debugPrint("SimpleBroadcast1Hop::processTaskREQ_ACKmessage::begin\n");
    TaskREQ t = payload->getTask();

    if (payload->getDest_ipAddress() == myAddress) {

        EV_INFO << myAddress << " - RECEIVED TASK ACK. " << t.getGen_ipAddress() << "-" << t.getId() << endl;

        debugPrint("SimpleBroadcast1Hop::processTaskREQ_ACKmessage::1\n");

        for (auto it = ackVector.begin(); it != ackVector.end(); )
        {
            debugPrint("SimpleBroadcast1Hop::processTaskREQ_ACKmessage::1_1\n");

            if ((t.getGen_ipAddress() == it->ft.task.getGen_ipAddress()) && (t.getId() == it->ft.task.getId())) {

                debugPrint("SimpleBroadcast1Hop::processTaskREQ_ACKmessage::1_1_1\n");

                int idx_erase = -1;
                int idx = 0;
                for (auto& el : it->non_ack_dests) {
                    if (payload->getSrc_ipAddress() == el) {
                        idx_erase = idx;
                    }
                    idx++;
                }

                debugPrint("SimpleBroadcast1Hop::processTaskREQ_ACKmessage::1_1_2\n");

                debugPrint("it->non_ack_dests size: %d \nit->non_ack_ttls size: %d \nidx_erase: %d \n \n",
                        (int)it->non_ack_dests.size(), (int)it->non_ack_ttls.size(), idx_erase);

                if (idx_erase >= 0) {
                    it->non_ack_dests.erase(it->non_ack_dests.begin() + idx_erase);
                    it->non_ack_ttls.erase(it->non_ack_ttls.begin() + idx_erase);
                }

                debugPrint("SimpleBroadcast1Hop::processTaskREQ_ACKmessage::1_1_3\n");

                if (it->non_ack_dests.size() == 0) {
                    // Remove the element from ackVector; erase returns an iterator
                    // pointing to the next element after the erased one
                    it = ackVector.erase(it);
                }
                else {
                    ++it;
                }

                debugPrint("SimpleBroadcast1Hop::processTaskREQ_ACKmessage::1_1_4\n");
            }
            else
            {
                debugPrint("SimpleBroadcast1Hop::processTaskREQ_ACKmessage::1_1_5\n");
                // Only advance if we haven't erased
                ++it;
            }

            debugPrint("SimpleBroadcast1Hop::processTaskREQ_ACKmessage::1_2\n");
        }

    }

    debugPrint("SimpleBroadcast1Hop::processTaskREQ_ACKmessage::end\n");

}

L3Address SimpleBroadcast1Hop::getBestNeighbor(TaskREQ& t)
{
    //return best neighbor IP address to the given task
    double rk;
    NodeData data = getMyNodeData();
    double outRk = calculateProgressiveScore(t, data);
    L3Address out = data.address;
    for (std::map<inet::L3Address, NodeData>::iterator it = nodeDataMap.begin(); it != nodeDataMap.end(); ++it) {
        L3Address ipAddr = it->first;
        data = it->second;
        if (isDeployFeasible(t, data)){
            rk = calculateProgressiveScore(t, data);
            if (rk >= outRk){
                out = ipAddr;
                outRk = rk;
            }
        }
    }
    return out;

}


void SimpleBroadcast1Hop::processTaskREQmessage(const Ptr<const TaskREQmessage>payload, L3Address srcAddr, L3Address destAddr)
{
    std::vector<L3Address> deployDest_out;
    std::vector<int> ttlDest_out;
    TaskREQ t = payload->getTask();
    bool to_ack = false;

    EV_INFO << myAddress << " - RECEIVED TASK. " << t.getGen_ipAddress() << "-" << t.getId() << "-" << payload->getIdReqMessage() << endl;


    //std::tuple<L3Address, uint32_t, uint32_t> packetId = std::make_tuple(t.getGen_ipAddress(), t.getId(), payload->getIdReqMessage());
    std::pair<L3Address, uint32_t> packetId = std::make_pair(t.getGen_ipAddress(), t.getId());
    //if (relayedPackets.find(packetId) == relayedPackets.end()) {
    // We not yet relayed this packet
    //relayedPackets.insert(packetId);

    //Hierarchical: - if I'm destination, deploy, else, forward
    //Progressive: - if I fit requirements, deploy, else, forward
    if (dissType == HIERARCHICAL || dissType == HIERARCHICAL_CHANGES) {
        //this is for hierarchical strategy
        for (int i = 0; i < payload->getDestDetailArraySize(); ++i) {
            DestDetail dd = payload->getDestDetail(i);
            L3Address finalDest = dd.getDest_ipAddress();
            L3Address nexthopDest = dd.getNextHop_ipAddress();
            if ((finalDest == myAddress) || (nexthopDest == myAddress)) {
                to_ack = true;
            }

        }

        for (int i = 0; i < payload->getDestDetailArraySize(); ++i) {
            DestDetail dd = payload->getDestDetail(i);
            L3Address finalDest = dd.getDest_ipAddress();
            L3Address nexthopDest = dd.getNextHop_ipAddress();
            int act_ttl = dd.getTtl();

            if (finalDest == myAddress) {
                if (extra_info_deploy_tasks.count(packetId) == 0) {
                    EV_INFO << "RECEIVED TASK. It's for me! Deploying..." << endl;
                    deployTaskHere(t);
                }
            }
            else if (nexthopDest == myAddress) {
                if (act_ttl > 1) {
                    deployDest_out.push_back(finalDest);
                    ttlDest_out.push_back(act_ttl - 1);
                }
            }
        }
    } else if (dissType == PROGRESSIVE){
        //Progressive strategy with Aggregated network knowledge
        //checks if this node meets the requirements, otherwise forwards to the best option (direction)
        deployDest_out.clear();
        ttlDest_out.clear();
        NodeData node = getMyNodeData();
        if (isDeployFeasible(t, node)){
            to_ack = true;
            deployTaskHere(t);
        } else {
            //choose one of my neighbors to forward the task
            DestDetail dd = payload->getDestDetail(0);
            L3Address finalDest = getBestNeighbor(t); //dd.getDest_ipAddress();
            if (finalDest == myAddress) {
                //Cannot deploy
                EV_INFO << "CANNOT deploy task!" << endl;
            } else {
                int act_ttl = dd.getTtl();
                deployDest_out.push_back(finalDest);
                ttlDest_out.push_back(act_ttl - 1);
            }
        }
    }


    //FORWARDING
    if (deployDest_out.size() > 0) {
        if (relayedPackets.find(packetId) == relayedPackets.end()) {
            relayedPackets.insert(packetId);


            EV_INFO << "RECEIVED TASK. Not for me but need to relay. Sending out" << endl;

            // sendTaskTo(deployDest_out, t, ttlDest_out);

            if (forwardingTask_queue.empty()) {
                scheduleClockEventAfter(uniform(0, maxForwardDelay), taskForwardMsg);
            }

            /*
                for (int i = 0; i < deployDest_out.size(); ++i) {
                    // Enqueue
                    Forwarding_Task ft;
                    ft.dests.push_back(deployDest_out[i]);
                    ft.task = t;
                    ft.ttls.push_back(ttlDest_out[i]);
                    forwardingTask_queue.push(ft);
                }
             */

            // Enqueue
            Forwarding_Task ft;
            ft.dests = deployDest_out;
            ft.task = t;
            ft.ttls = ttlDest_out;
            ft.numberOfSending = 0;
            forwardingTask_queue.push(ft);
        }
        else {
            EV_INFO << "TASK from " << t.getGen_ipAddress() << ", ID: " << t.getId() << " already managed. Skipping this" << endl;
        }

    }

    if ((to_ack) && (ack_func)) {

        EV_INFO << "Sending ACK for this TASK" << endl;

        std::ostringstream str;
        str << "Ack-" << t.getGen_ipAddress().str() << "-" << t.getId();
        Packet *packet = new Packet(str.str().c_str());
        if (dontFragment)
            packet->addTag<FragmentationReq>()->setDontFragment(true);

        //const auto& payload = createPayloadForTask(dest, destAddr,  task, ttl);
        //const auto& payload = createPayloadForTask(dest_next_ttl, task);

        const auto& payload = makeShared<TaskREQ_ACKmessage>();

        payload->setChunkLength(B(par("messageLength"))); // TODO

        payload->setTask(t);
        payload->setSrc_ipAddress(myAddress);
        payload->setDest_ipAddress(srcAddr);


        EV_INFO << "Sending ACK for this TASK. SRC: " << myAddress << "; DEST: " << srcAddr << endl;


        packet->insertAtBack(payload);

        socket.sendTo(packet, L3Address("255.255.255.255"), destPort);

    }

//    for (int i = 0; i < payload->getDestDetailArraySize(); ++i) {
//        DestDetail dd = payload->getDestDetail(i);
//        L3Address finalDest = dd.getDest_ipAddress();
//        L3Address nexthopDest = dd.getNextHop_ipAddress();
//        int act_ttl = dd.getTtl();
//
//        if (finalDest == myAddress) {
//            // TODO here you need to choose among hierarchical vs progressive
//
//            EV_INFO << "RECEIVED TASK. It's for me! Deploying..." << endl;
//
//            //L3Address deployDest = checkDeployDestination(task);
//
//            // hierarchical
//            // TODO check if task already deployed
//            deployTaskHere(t);
//        }
//        else if (nexthopDest == myAddress) {
//            if (act_ttl > 1) {
//                deployDest_out.push_back(finalDest);
//                ttlDest_out.push_back(act_ttl - 1);
//            }
//        }
//    }
//
//    if (deployDest_out.size() > 0) {
//        //std::tuple<L3Address, uint32_t, uint32_t> packetId = std::make_tuple(t.getGen_ipAddress(), t.getId(), payload->getIdReqMessage());
//        std::pair<L3Address, uint32_t> packetId = std::make_pair(t.getGen_ipAddress(), t.getId());
//        if (relayedPackets.find(packetId) == relayedPackets.end()) {
//            // We not yet relayed this packet
//            relayedPackets.insert(packetId);
//
//            EV_INFO << "RECEIVED TASK. Not for me but need to relay. Sending out" << endl;
//
//            sendTaskTo(deployDest_out, t, ttlDest_out);
//        }
//    }

//    L3Address finalDest = payload->getDest_ipAddress();
//    L3Address nexthopDest = payload->getNextHop_ipAddress();
//    TaskREQ t = payload->getTask();
//    int act_ttl = payload->getTtl();
//
//    EV_INFO << "RECEIVED TASK with final dest: " << finalDest << ". My address is: " << myAddress << endl;
//
//    if (finalDest == myAddress) {
//        // TODO here you need to choose among hierarchical vs progressive
//
//        //L3Address deployDest = checkDeployDestination(task);
//
//        // hierarchical
//        deployTaskHere(t);
//    }
//    else if (nexthopDest == myAddress) {
//
//        if (act_ttl > 1) {
//
//            //Check if already relayed
//            // Check if packetId is already in the set
//            //std::pair<L3Address, uint32_t> packetId = std::make_pair(t.getGen_ipAddress(), t.getId());
//            std::tuple<L3Address, uint32_t, uint32_t> packetId = std::make_tuple(t.getGen_ipAddress(), t.getId(), payload->getIdReqMessage());
//            if (relayedPackets.find(packetId) == relayedPackets.end()) {
//
//                // We not yet relayed this packet
//                relayedPackets.insert(packetId);
//
//                EV_INFO << "RECEIVED TASK. Not for me but need to relay. Sending out" << endl;
//                //manageTask(payload->getTask());
//                //L3Address deployDest = finalDest;
//                sendTaskTo(finalDest, t, act_ttl - 1);
//            }
//        }
//    }
}

void SimpleBroadcast1Hop::generateNewTask()
{
    TaskREQ newTask = parseTask();  //here the function that PARSE the SG script

    EV_INFO << "Generating new TASK: " << newTask << endl;

    std::pair<L3Address, uint32_t> packetId = std::make_pair(newTask.getGen_ipAddress(), newTask.getId());
    relayedPackets.insert(packetId);

    generatedTask_list.push_back(newTask);

    manageNewTask(newTask, true);
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
        else if (msg == taskForwardMsg){

            switch (taskForwardMsg->getKind()) {
            case FORWARD:
                forwardTask();
                //scheduleClockEventAfter(truncnormal(mean_tf, stddev_tf), taskForwardMsg);

                if (!forwardingTask_queue.empty()) {
                    // Generate a random delay uniformly in [0, maxDelay]
                    double randomDelay = uniform(0, maxForwardDelay);
                    // Schedule the self-message after this delay
                    scheduleAfter(randomDelay, taskForwardMsg);
                }
                break;

            default:
                throw cRuntimeError("Invalid kind %d in self task forwarding message", (int)taskForwardMsg->getKind());
            }


        }
        else if (msg == taskAckMsg){

            switch (taskAckMsg->getKind()) {
            case ACK_CHECK:
                ackTask();

                scheduleClockEventAfter(truncnormal(1, 0.01), taskAckMsg);
                break;

            default:
                throw cRuntimeError("Invalid kind %d in self task forwarding message", (int)taskForwardMsg->getKind());
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

    data.radius = payload->getRadius();

    // Store or update the data in the map
    nodeDataMap[srcAddr] = data;

    updateRadius();

    if (dissType == HIERARCHICAL) {
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
                    data_nest.radius = nf.getRadius();

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


    //auto bytesChunk = pk->peekAllAsBytes();
    //EV_INFO << "Raw packet bytes: " << bytesChunk->str() << endl;

    EV_INFO << "Packet name: " << pk->getName() << ", length: " << pk->getTotalLength() << "\n";
    EV_INFO << "Tags:\n";
    for (int i = 0; i < pk->getNumTags(); i++) {
        auto tag = pk->getTag(i);
        EV_INFO << "  Tag " << i << ": " << tag->str() << "\n";
    }

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

    std::string s = pk->getName();

    EV_INFO << myAddress << " - Received packet: " << s << ", srcAddr: " << srcAddr << "\n";

    if ((srcAddr != loopbackAddress) && (srcAddr != myAddress)) {

//        auto taskPayload = pk->peekAtBack<TaskREQmessage>(b(-1), 0);
//        if (taskPayload != nullptr) {
//            // It's indeed a TaskREQmessage chunk
//            processTaskREQmessage(taskPayload, srcAddr, destAddr);
//        }
//        else {
//            auto heartbeatPayload = pk->peekAtBack<Heartbeat>(b(-1), 0);
//            if (heartbeatPayload != nullptr) {
//                // It's a Heartbeat
//                processHeartbeat(heartbeatPayload, srcAddr, destAddr);
//            }
//            else {
//                EV_WARN << "No recognized chunk at the back.\n";
//            }
//        }

        // Process here
        // Check if the packet contains Heartbeat data
        //if ((s.rfind("Heartbeat", 0)) && (pk->hasData<Heartbeat>())) {

        if (s.rfind("Heartbeat", 0) == 0) {
            // Extract the Heartbeat payload
            const auto& payload = pk->peekData<Heartbeat>();
            processHeartbeat(payload, srcAddr, destAddr);
        } else if (s.rfind("Changes", 0) == 0) {
            // Extract the ChangesBlock payload
            const auto& payload = pk->peekData<ChangesBlock>();
            processChangesBlock(payload, srcAddr, destAddr);
        } else if (s.rfind("Task", 0) == 0) {
            // Extract the TaskREQmessage payload
            const auto& payload = pk->peekData<TaskREQmessage>();

            processTaskREQmessage(payload, srcAddr, destAddr);
        }
        else if (s.rfind("Ack", 0) == 0) {
            // Extract the TaskREQ_ACKmessage payload
            const auto& payload = pk->peekData<TaskREQ_ACKmessage>();

            processTaskREQ_ACKmessage(payload, srcAddr, destAddr);
        }
        else {
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
    socket.destroy(); // in real operating systems, program crash detected by OS and OS closes sockets of crashed programs.
}


Ptr<ChangesBlock> SimpleBroadcast1Hop::createChangesPayload()
{

    //update NodeInfo with assigned tasks

    double compActUsage = 0;
    double memActUsage = 0;
    bool lockedGPU = false;
    bool lockedCamera = false;
    bool lockedFly = false;
    for (size_t i = 0; i < assignedTask_list.size(); ++i) {
        if ((simTime() < assignedTask_list[i].getEnd_timestamp()) && (simTime() >= assignedTask_list[i].getStart_timestamp())){
            compActUsage += assignedTask_list[i].getReqCPU();
            memActUsage += assignedTask_list[i].getReqMemory();
            if (assignedTask_list[i].getLockGPU()) lockedGPU = true;
            if (assignedTask_list[i].getLockCamera()) lockedCamera = true;
            if (assignedTask_list[i].getReq_lock_flyengine()) lockedFly = true;
        }
    }


    const auto& payload = makeShared<ChangesBlock>();

    //getting changes of THIS node...

    uint32_t seq = lastReport.getSequenceNumber() + 1;

    double deltaComp = 5; // acceptable delta without notification
    double deltaMemory = 5;
    double deltaCoord = 2; // percent (%)

    //comparing actual values with last reported ones over deltas
    Change ch;
    ch.setSequenceNumber(seq);
    ch.setIpAddress(myAddress);
    ch.setNextHop_address(myAddress);
    ch.setHops(0);

    //ch.setNum_hops(0);
    int stksize = stChanges.size();
    if ((abs((lastReport.getCoord_x() - mob->getCurrentPosition().x)/lastReport.getCoord_x()) * 100) > deltaCoord ||
            (abs((lastReport.getCoord_y() - mob->getCurrentPosition().y)/lastReport.getCoord_y()) * 100)  > deltaCoord  ) {
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

    if (lastReport.getCompMaxUsage() != computationalPower) {
        //report Max CPU
        ch.setParammeter(fldMaxCPU);
        ch.setValue(computationalPower);
        lastReport.setCompMaxUsage(computationalPower);
        stChanges.push_back(ch);
    }

    if (lastReport.getMemoryMaxUsage() != availableMaxMemory) {
        //report MAX Memory
        ch.setParammeter(fldMaxMEM);
        ch.setValue(availableMaxMemory);
        lastReport.setMemoryMaxUsage(availableMaxMemory);
        stChanges.push_back(ch);

    }

    if (abs(lastReport.getCompActUsage() - compActUsage) > deltaComp) {
        //report CPU
        ch.setParammeter(fldActCPU);
        ch.setValue(compActUsage);
        lastReport.setCompActUsage(compActUsage);
        stChanges.push_back(ch);
    }

    if (abs(lastReport.getMemoryActUsage() - memActUsage) > deltaMemory) {
        //report Memory
        ch.setParammeter(fldActMEM);
        ch.setValue(memActUsage);
        lastReport.setMemoryActUsage(memActUsage);
        stChanges.push_back(ch);

    }

    if (lastReport.getHasCamera() != hasCamera) {
        //report Camera
        ch.setParammeter(fldCAM);
        ch.setValue((hasCamera ? 1 : 0));
        lastReport.setHasCamera(hasCamera);
        stChanges.push_back(ch);
    }

    if (lastReport.getHasGPU() != hasGPU) {
        //report GPU
        ch.setParammeter(fldGPU);
        ch.setValue((hasGPU? 1 : 0));
        lastReport.setHasGPU(hasGPU);
        stChanges.push_back(ch);
    }

    if (lastReport.getLockedCamera() != lockedCamera) {
        //report locked camera
        ch.setParammeter(fldLkCAM);
        ch.setValue((lockedCamera ? 1 : 0));
        lastReport.setLockedCamera(lockedCamera);
        stChanges.push_back(ch);
    }

    if (lastReport.getLockedFly() != lockedFly) {
        //report locked fly
        ch.setParammeter(fldLkFLY);
        ch.setValue((lockedFly ? 1 : 0));
        lastReport.setLockedFly(lockedFly);
        stChanges.push_back(ch);

    }

    if (lastReport.getLockedGPU() != lockedGPU) {
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
    while (!stChanges.empty() && i<100){
        Change cht = stChanges.at(0);
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
    uint32_t s = sizeof(ChangesBlock) + payload->getChangesListArraySize() * sizeof(Change) + 1;

    //payload->setChunkLength(B(s));
    payload->setChunkLength(B(par("messageLength")));

    return payload;


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
                nd.nextHop_address = ch.getNextHop_address();
                nd.num_hops = ch.getHops() + 1;
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
                case fldRadius:
                    nd.radius = ch.getValue();
                    break;
                default:
                    EV_ERROR << "new unidentified field " << std::endl;
                    break;
                }
                nd.lastSeqNumber[ch.getParammeter()] = ch.getSequenceNumber();
                nodeDataMap[node_addr] = nd;
            }
            addChange(ch);
        }

    }
    //EV_INFO << myAddress << " nodeDataMap size: " << nodeDataMap.size() << std::endl;
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






} // namespace inet

