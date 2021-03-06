#include <ros/ros.h>

#include <std_msgs/String.h>
#include <std_srvs/Trigger.h>

#include <memory>
#include <unordered_map>

#include "agility.hpp"
#include "ariac_agv.hpp"
#include "arm.hpp"

#define NUM_AGVS 4

using AriacAgvMap = std::unordered_map<std::string, std::shared_ptr<AriacAgv>>;

namespace {
    std::string build_part_frame(const std::string& product_type, const int camera_index, const int counter)
    {
        return std::string("logical_camera_")
            + std::to_string(camera_index)
            + "_"
            + product_type
            + "_"
            + std::to_string(counter)
            + "_frame"
        ;
    }

    bool does_frame_exist(const std::string& part_in_camera_frame, const double timeout)
    {
        bool rc = true;
        static tf2_ros::Buffer tfBuffer;
        static tf2_ros::TransformListener tfListener(tfBuffer);
        try {
            tfBuffer.lookupTransform(
                "world",
                part_in_camera_frame,
                ros::Time(0),
                ros::Duration(timeout)
            );
        } catch (tf2::TransformException& ex) {
            rc = false;
        }
        return rc;
    }

    void cater_higher_priority_order_if_necessary(const AriacAgvMap& agv_map,
                                                  AgilityChallenger* const agility,
                                                  Arm* const arm,
                                                  const int current_order_priority);

    void cater_kitting_shipments(const AriacAgvMap& agv_map,
                                 AgilityChallenger* const agility,
                                 Arm* const arm,
                                 const int order_priority,
                                 std::vector<nist_gear::KittingShipment>& kitting_shipments)
    {
        // Ignore request if there are no kitting shipments
        if (kitting_shipments.empty())
        {
            return;
        }

        ROS_INFO_STREAM("Catering "
                        << kitting_shipments.size()
                        << " kitting shipments from order with priority of "
                        << order_priority);

        int counter = 0;
        // parse each kitting shipment
        for (const auto& ks : kitting_shipments)
        {
            if (ks.products.empty())
            {
                ROS_FATAL_STREAM("Kitting shipment had no products?");
                ros::shutdown();
                return;
            }

            std::vector<std::pair<nist_gear::Product, std::vector<int>>> product_location_pairs;
            for (const auto& product : ks.products)
            {
                const std::vector<int> bin_indices = agility->get_camera_indices_of(product.type);
                if (bin_indices.empty())
                {
                    ROS_FATAL_STREAM(
                        "No matching part '"
                        << product.type
                        << "' found by any logical camera with contents "
                        << agility->get_logical_camera_contents()
                    );
                    ros::shutdown();
                    return;
                }
                else
                {
                    product_location_pairs.push_back(std::make_pair(
                        product,
                        bin_indices
                    ));
                }
            }

            // loop through each product in this shipment
            int product_placed_in_shipment = 0;
            for (int product_idx = 0; product_idx < product_location_pairs.size();)
            {
                nist_gear::Product product;
                std::vector<int> bin_indices;
                std::tie(product, bin_indices) = product_location_pairs[product_idx];

                // Even if we did not increment product_idx, increment counter
                counter++;

                for (auto iter = bin_indices.cbegin(); iter != bin_indices.cend(); ++iter)
                {
                    const std::string part_frame = build_part_frame(product.type, *iter, counter);
                    if (!does_frame_exist(part_frame, 0.5))
                    {
                        continue;
                    }

                    // Move the part from where it is to the AGV bed
                    ROS_INFO_STREAM("Moving part '" << product.type << "' to '" << ks.agv_id << "' (" << part_frame << ")");
                    arm->movePart(product.type, part_frame, product.pose, ks.agv_id);
                    ROS_INFO_STREAM("Placed part '" << product.type << "' at '" << ks.agv_id << "'");

                    // Give an opportunity for higher priority orders
                    cater_higher_priority_order_if_necessary(agv_map, agility, arm, order_priority);

                    // If this part is faulty, move it
                    geometry_msgs::Pose faulty_part_pick_frame;
                    if (agility->get_agv_faulty_part(faulty_part_pick_frame))
                    {
                        // TODO: tweak all the stops/sleeps?
                        ROS_WARN_STREAM("Placed part is faulty!");
                        arm->goToPresetLocation(ks.agv_id);
                        if (arm->pickPart(product.type, faulty_part_pick_frame))
                        {
                            ros::Duration(2.0).sleep();
                            arm->goToPresetLocation(ks.agv_id);
                            arm->goToPresetLocation("home2");
                            ros::Duration(0.5).sleep();
                            arm->deactivateGripper();
                        }
                        else
                        {
                            ROS_ERROR_STREAM("Failed to pick the faulty part");
                        }

                        // Give an opportunity for higher priority orders
                        cater_higher_priority_order_if_necessary(agv_map, agility, arm, order_priority);
                    }
                    else
                    {
                        // We successfully placed the product
                        product_idx++;
                        break;
                    }
                }
            }

            // If we're here, we have placed all products in this shipment
            ros::Duration(1.0).sleep();
            const auto agv_iter = agv_map.find(ks.agv_id);
            if (agv_map.cend() != agv_iter)
            {
                const std::shared_ptr<AriacAgv> agv = agv_iter->second;
                if (agv->is_ready_to_deliver())
                {
                    agv->submit_shipment(
                        ks.station_id,
                        ks.shipment_type
                    );
                }
                else
                {
                    ROS_ERROR_STREAM("AGV with ID " << ks.agv_id << " is not ready to ship");
                }
            }
            else
            {
                ROS_FATAL_STREAM("Unknown AGV with ID " << ks.agv_id);
                ros::shutdown();
                return;
            }
        }
    }

    void cater_order(const AriacAgvMap& agv_map,
                     AgilityChallenger* const agility,
                     Arm* const arm,
                     const int order_priority,
                     nist_gear::Order& order)
    {
        cater_kitting_shipments(
            agv_map,
            agility,
            arm,
            order_priority,
            order.kitting_shipments
        );
    }

    void cater_higher_priority_order_if_necessary(const AriacAgvMap& agv_map,
                                                  AgilityChallenger* const agility,
                                                  Arm* const arm,
                                                  const int current_order_priority)
    {
        if (agility->higher_priority_order_requested(current_order_priority))
        {
            ROS_INFO_STREAM("Catering higher priority order...");
            int new_order_priority;
            nist_gear::Order new_order;
            new_order_priority = agility->consume_pending_order(new_order);
            cater_order(agv_map, agility, arm, new_order_priority, new_order);
            ROS_INFO_STREAM("Finished higher priority order, returning to previous order");
        }
    }
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "arm_controller");
    ros::NodeHandle nh;

    // Create interfaces to each of the AGVs
    AriacAgvMap agv_map;
    for (int i = 0; i < NUM_AGVS; i++)
    {
        // AGV topics use identifiers in the range [1,NUM_AGVS], but this loop
        // is [0,NUM_AGVS-1], so add 1 to all indices when creating them here
        auto agv = std::shared_ptr<AriacAgv>(new AriacAgv(&nh, i+1));
        agv_map[agv->get_id()] = agv;
    }

    ros::AsyncSpinner spinner(0);
    spinner.start();

    AgilityChallenger agility(&nh);
    Arm arm;

    //
    // Start the competition
    //

    ros::Duration wait_for_competition_state(0.1);
    bool competition_state_valid = false;
    std::string competition_state;
    ros::Subscriber competition_state_sub = nh.subscribe<std_msgs::String>(
        "/ariac/competition_state",
        1,
        [&](const std_msgs::String::ConstPtr& msg) 
        {
            competition_state = msg->data;
            competition_state_valid = true;
        }
    );

    // Spin the node until we've collected the competition state
    while (!competition_state_valid)
    {
        wait_for_competition_state.sleep();
        ros::spinOnce();
    }

    // If the competition has not started, we place a request to start it
    if (competition_state == "go")
    {
        ROS_INFO_STREAM("Competition is already started.");
    }
    else
    {
        // Create the client
        assert(competition_state == "init");
        ros::ServiceClient start_competition_scl = nh.serviceClient<std_srvs::Trigger>("/ariac/start_competition");
        // Wait a very long time, because this service server may take a while
        // to be created
        assert(start_competition_scl.waitForExistence(ros::Duration(30.0)));

        // Place the request
        std_srvs::Trigger start_competition_srv;
        if (!(start_competition_scl.call(start_competition_srv) && start_competition_srv.response.success))
        {
            ROS_ERROR_STREAM("Failed to start the competition: '"<< start_competition_srv.response.message);
            return 1;
        }

        // Wait a little and collect the updated competition state
        competition_state_valid = false;
        ros::Duration(0.25).sleep();
        ros::spinOnce();
        if (competition_state == "go")
        {
            ROS_INFO_STREAM("Competition was started.");
        }
        else
        {
            ROS_ERROR_STREAM("Competition state did not update as expected: '"<< competition_state);
            return 2;
        }
    }

    //
    // If we're here, the competition has started
    //

    arm.goToPresetLocation("home1");
    arm.goToPresetLocation("home2");

    int current_order_priority;
    nist_gear::Order current_order;
    ros::Duration rate(0.1);
    while (ros::ok())
    {
        current_order_priority = agility.consume_pending_order(current_order);
        if (0 != current_order_priority)
        {
            cater_order(
                agv_map,
                &agility,
                &arm,
                current_order_priority,
                current_order
            );
        }
        else
        {
            rate.sleep();
        }
    }

    return 0;
}
