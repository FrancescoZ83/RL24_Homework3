// Copyright  (C)  2007  Francois Cauwe <francois at cauwe dot org>
 
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
 
#include <stdio.h>
#include <iostream>
#include <chrono>
#include <cstdlib>
#include <memory>

#include "std_msgs/msg/float64_multi_array.hpp"
#include "sensor_msgs/msg/joint_state.hpp"

#include "rclcpp/rclcpp.hpp"
#include "rclcpp/wait_for_message.hpp"

#include "kdl_robot.h"
#include "kdl_control.h"
#include "kdl_planner.h"
#include "kdl_parser/kdl_parser.hpp"

#include "tf2_ros/buffer.h"
#include "tf2_ros/transform_listener.h"
#include "geometry_msgs/msg/transform_stamped.hpp"
#include "tf2_kdl/tf2_kdl.hpp"
#include "tf2/transform_datatypes.h"
#include "tf2_geometry_msgs/tf2_geometry_msgs.hpp"
 
using namespace KDL;
using FloatArray = std_msgs::msg::Float64MultiArray;
using namespace std::chrono_literals;

class Iiwa_pub_sub : public rclcpp::Node
{
    public:
        Iiwa_pub_sub()
        : Node("ros2_kdl_node"), 
        node_handle_(std::shared_ptr<Iiwa_pub_sub>(this))
        {
            // declare cmd_interface parameter (position, velocity)
            declare_parameter("cmd_interface", "velocity"); // defaults to "position"
            get_parameter("cmd_interface", cmd_interface_);
            
            // other parameters
            declare_parameter("traj_type", "no_traj");
            get_parameter("traj_type", traj_type_);
            declare_parameter("cont_type", "jnt");
            get_parameter("cont_type", cont_type_);
            
            declare_parameter("task", "positioning");
            get_parameter("task", task_);
            
            RCLCPP_INFO(get_logger(),"Current cmd interface is: '%s'", cmd_interface_.c_str());
            RCLCPP_INFO(get_logger(),"Current trajectory type is: '%s'", traj_type_.c_str());

            if (!(cmd_interface_ == "velocity" || cmd_interface_ == "effort"))
            {
                RCLCPP_INFO(get_logger(),"Selected cmd interface is not valid!"); return;
            }
            if (!(traj_type_ == "lin_pol" || traj_type_ == "lin_trap" || traj_type_ == "cir_pol" || traj_type_ == "cir_trap" || traj_type_ == "no_traj"))
            {
                RCLCPP_INFO(get_logger(),"Selected trajectory type is not valid!"); return;
            }
            if (!(cont_type_ == "jnt" || cont_type_ == "op"))
            {
                RCLCPP_INFO(get_logger(),"Selected control type is not valid!"); return;
            }
            if (!(task_ == "positioning" || task_ == "look_at_point"))
            {
                RCLCPP_INFO(get_logger(),"Selected task is not valid!"); return;
            }

            iteration_ = 0;
            t_ = 0;
            joint_state_available_ = false; 

            // retrieve robot_description param
            auto parameters_client = std::make_shared<rclcpp::SyncParametersClient>(node_handle_, "robot_state_publisher");
            while (!parameters_client->wait_for_service(1s)) {
                if (!rclcpp::ok()) {
                    RCLCPP_ERROR(this->get_logger(), "Interrupted while waiting for the service. Exiting.");
                    rclcpp::shutdown();
                }
                RCLCPP_INFO(this->get_logger(), "service not available, waiting again...");
            }
            auto parameter = parameters_client->get_parameters({"robot_description"});

            // create KDLrobot structure
            KDL::Tree robot_tree;
            if (!kdl_parser::treeFromString(parameter[0].value_to_string(), robot_tree)){
                std::cout << "Failed to retrieve robot_description param!";
            }
            robot_ = std::make_shared<KDLRobot>(robot_tree);  
            
            // Create joint array
            unsigned int nj = robot_->getNrJnts();
            KDL::JntArray q_min(nj), q_max(nj);
            q_min.data << -2.96,-2.09,-2.96,-2.09,-2.96,-2.09,-2.96; //-2*M_PI,-2*M_PI; // TODO: read from urdf file
            q_max.data <<  2.96,2.09,2.96,2.09,2.96,2.09,2.96; //2*M_PI, 2*M_PI; // TODO: read from urdf file          
            robot_->setJntLimits(q_min,q_max);            
            joint_positions_.resize(nj); 
            joint_velocities_.resize(nj);
            joint_efforts_.resize(nj);
            
            // joint references for effort control
            dpos.resize(nj);
            dvel.resize(nj);
            dacc.resize(nj);
            
            // Vision Task: joint references
            dpos_vis.resize(nj);
            dvel_vis.resize(nj);
            dacc_vis.resize(nj);

            // Subscriber to jnt states
            jointSubscriber_ = this->create_subscription<sensor_msgs::msg::JointState>(
                "/joint_states", 10, std::bind(&Iiwa_pub_sub::joint_state_subscriber, this, std::placeholders::_1));

            // Wait for the joint_state topic
            while(!joint_state_available_){
                RCLCPP_INFO(this->get_logger(), "No data received yet! ...");
                rclcpp::spin_some(node_handle_);
            }

            // Update KDLrobot object
            robot_->update(toStdVector(joint_positions_.data),toStdVector(joint_velocities_.data));
            KDL::Frame f_T_ee = KDL::Frame::Identity();
            robot_->addEE(f_T_ee);
            robot_->update(toStdVector(joint_positions_.data),toStdVector(joint_velocities_.data));

            // Compute EE frame
            init_cart_pose_ = robot_->getEEFrame();

            // Compute IK
            KDL::JntArray q(nj);
            robot_->getInverseKinematics(init_cart_pose_, q);

            // Initialize controller
            KDLController controller_(*robot_);

            // EE's trajectory initial position (just an offset)
            Eigen::Vector3d init_position(Eigen::Vector3d(init_cart_pose_.p.data) + Eigen::Vector3d(0,0,0.1));

            // EE's trajectory end position (different x and opposite y)
            Eigen::Vector3d end_position; end_position << init_position[0]+0.1, -init_position[1], init_position[2];
            
            // Vision Task: Initialization of tf2 parameters to get the aruco pose
            
            tf_buffer_ = std::make_shared<tf2_ros::Buffer>(this->get_clock());
            tf_listener_ = std::make_shared<tf2_ros::TransformListener>(*tf_buffer_);
            
            // Vision Task: Initialize the aruco frame as if the initial pose is the desired one
            
            KDL::Frame inverse_rotation_frame2(KDL::Rotation::RotZ(-3.14), KDL::Vector::Zero());
            KDL::Frame inverse_rotation_frame(KDL::Rotation::RotX(-3.14), KDL::Vector::Zero());
            KDL::Frame inverse_translation_frame(KDL::Rotation::Identity(), KDL::Vector(0.0, 0.0, -positioning_offset));
            aruco_frame = init_cart_pose_ * inverse_rotation_frame2 * inverse_rotation_frame * inverse_translation_frame;
            camera_frame = init_cart_pose_;
    
            // Plan trajectory
            double traj_duration = 5, acc_duration = 0.5, t = 0.0, radius=0.3;
            planner_linear = KDLPlanner(traj_duration, init_position, end_position);
            planner_circle = KDLPlanner(traj_duration, init_position, radius);
            
            // Retrieve the first trajectory point
            
            trajectory_point p;
            
            if(traj_type_ == "lin_pol" || traj_type_ == "no_traj"){
                p = planner_linear.compute_trajectory_linear(t);
            }else if(traj_type_ == "lin_trap"){
                p = planner_linear.compute_trajectory_linear(t, acc_duration);
            }else if(traj_type_ == "cir_pol"){
                p = planner_circle.compute_trajectory_circle(t);
            }else if(traj_type_ == "cir_trap"){
                p = planner_circle.compute_trajectory_circle(t, acc_duration);
            }
            
            // Definition of desired orientation
            KDL::Frame des_pos_rot_; des_pos_rot_.M = init_cart_pose_.M; des_pos_rot_.p = toKDL(p.pos);
            Eigen::VectorXd des_vel_rot_ = Eigen::VectorXd::Zero(3);
            Eigen::VectorXd des_acc_rot_ = Eigen::VectorXd::Zero(3);
            
            // Initialization of joint ref. for effort control (needed for numerical integration)
            dvel.data = Eigen::VectorXd::Zero(7);
            robot_->getInverseKinematics(des_pos_rot_, dpos);
            
            dvel_vis.data = dvel.data;
            robot_->getInverseKinematics(init_cart_pose_, dpos_vis);
            
            
            if(cmd_interface_ == "velocity"){
                // Create cmd publisher
                cmdPublisher_ = this->create_publisher<FloatArray>("/velocity_controller/commands", 10);
                timer_ = this->create_wall_timer(std::chrono::milliseconds(freq_ms), 
                                            std::bind(&Iiwa_pub_sub::cmd_publisher, this));
            
                // Send joint velocity commands
                for (long int i = 0; i < joint_velocities_.data.size(); ++i) {
                    desired_commands_[i] = joint_velocities_(i);
                }
            }
            // effort control publisher 
            else if(cmd_interface_ == "effort"){
                cmdPublisher_ = this->create_publisher<FloatArray>("/effort_controller/commands", 10);
                timer_ = this->create_wall_timer(std::chrono::milliseconds(freq_ms), 
                                            std::bind(&Iiwa_pub_sub::cmd_publisher, this));
                for (long int i = 0; i < joint_efforts_.data.size(); ++i) {
                    desired_commands_[i] = joint_efforts_(i);
                }
            }

            // Create msg and publish
            std_msgs::msg::Float64MultiArray cmd_msg;
            cmd_msg.data = desired_commands_;
            cmdPublisher_->publish(cmd_msg);

            RCLCPP_INFO(this->get_logger(), "Starting trajectory execution ...");
        }

    private:
    
        void get_tf(KDL::Frame& frame_out, const std::string& tf_base, const std::string& tf_end){
            try{
                geometry_msgs::msg::TransformStamped temp_pose;
                temp_pose = tf_buffer_->lookupTransform(tf_base, tf_end, rclcpp::Time(0), std::chrono::milliseconds(100));
                
                // Convert to tf2::Transform
                tf2::Transform tf3d;
                tf2::fromMsg(temp_pose.transform, tf3d);  // Convert to tf2::Transform
                // Extract translation and rotation
                KDL::Vector translation(tf3d.getOrigin().x(), tf3d.getOrigin().y(), tf3d.getOrigin().z());
                KDL::Rotation rotation = KDL::Rotation::Quaternion(
                tf3d.getRotation().x(), tf3d.getRotation().y(),
                tf3d.getRotation().z(), tf3d.getRotation().w());
                // Create KDL::Frame from translation and rotation
                KDL::Frame frame_temp(rotation, translation);
                
                frame_out = frame_temp;
                
            }catch(const tf2::TransformException &){frame_out=frame_out;}
        }

        void cmd_publisher(){

            iteration_ = iteration_ + 1;
            
            // define trajectory
            double total_time = 5;
            int trajectory_len = total_time * 1000/(freq_ms);
            double acc_duration = 0.5;
            int loop_rate = trajectory_len / total_time;
            double dt = 1.0 / loop_rate;
            t_+=dt;
            
            // Vision task: Compute desired pose for positioning task
            
            get_tf(aruco_frame, "world", "aruco_marker_frame");
            KDL::Frame cartpos = robot_->getEEFrame();
            
            if(task_ == "positioning"){
            
                KDL::Frame translation_frame(KDL::Rotation::Identity(), KDL::Vector(0.0, 0.0, positioning_offset));
                KDL::Frame rotation_frame(KDL::Rotation::RotX(3.14), KDL::Vector::Zero());
                KDL::Frame rotation_frame2(KDL::Rotation::RotZ(3.14), KDL::Vector::Zero());
                KDL::Frame desired_frame = aruco_frame * translation_frame * rotation_frame * rotation_frame2;
                
                if(cmd_interface_ == "velocity"){
                    Eigen::Vector3d error = computeLinearError(Eigen::Vector3d(desired_frame.p.data), Eigen::Vector3d(cartpos.p.data));
                    Eigen::Vector3d o_error = computeOrientationError(toEigen(desired_frame.M), toEigen(cartpos.M));
                    
                    Vector6d cartvel; cartvel << 5*error, 3*o_error;
                    joint_velocities_.data = pseudoinverse(robot_->getEEJacobian().data)*cartvel;
                    joint_positions_.data = joint_positions_.data + joint_velocities_.data*dt;
                    
                }else if(cmd_interface_ == "effort"){
                    
                    if(cont_type_ == "jnt"){
                    
                        robot_->getInverseKinematics(desired_frame, dpos_vis);
                        dvel_vis.data = Eigen::VectorXd::Zero(robot_->getNrJnts());
                        dacc_vis.data = Eigen::VectorXd::Zero(robot_->getNrJnts());
                        joint_efforts_.data = controller_->idCntr(dpos_vis, dvel_vis, dacc_vis, KP_j, KD_j, *robot_) - robot_->getGravity();
                        
                    }else if(cont_type_ == "op"){
                        
                        KDL::Frame d_pos = desired_frame;
                        KDL::Twist d_vel = toKDLTwist(Eigen::VectorXd::Zero(6));
                        KDL::Twist d_acc = toKDLTwist(Eigen::VectorXd::Zero(6));
                        joint_efforts_.data = controller_->idCntr(d_pos, d_vel, d_acc, KP_o, KD_o, *robot_, lambda_op) - robot_->getGravity();
                    }
                }
                
            }
            
            // Trajectory Computation
            
            
            /*
            
            
            if (t_ < total_time){

                // Retrieve the trajectory point
                
                trajectory_point p;
                if(traj_type_ == "lin_pol"){
                    p = planner_linear.compute_trajectory_linear(t_);
                }else if(traj_type_ == "lin_trap"){
                    p = planner_linear.compute_trajectory_linear(t_, acc_duration);
                }else if(traj_type_ == "cir_pol"){
                    p = planner_circle.compute_trajectory_circle(t_);
                }else if(traj_type_ == "cir_trap"){
                    p = planner_circle.compute_trajectory_circle(t_, acc_duration);
                }
                
                KDL::Frame des_pos_rot_; des_pos_rot_.M = init_cart_pose_.M;
                Eigen::VectorXd des_vel_rot_ = Eigen::VectorXd::Zero(3);
                Eigen::VectorXd des_acc_rot_ = Eigen::VectorXd::Zero(3);

                // Compute EE frame
                KDL::Frame cartpos = robot_->getEEFrame();          

                // Compute desired Frame
                KDL::Frame desFrame; desFrame.M = cartpos.M; desFrame.p = toKDL(p.pos);
                
                // compute errors
                Eigen::Vector3d error = computeLinearError(p.pos, Eigen::Vector3d(cartpos.p.data));
                Eigen::Vector3d o_error = computeOrientationError(toEigen(init_cart_pose_.M), toEigen(cartpos.M));
                std::cout << "The error norm is : " << error.norm() << std::endl;

                if(cmd_interface_ == "velocity"){

                    // Compute differential IK
                    Vector6d cartvel; cartvel << p.vel + 5*error, o_error;
                    joint_velocities_.data = pseudoinverse(robot_->getEEJacobian().data)*cartvel;
                    joint_positions_.data = joint_positions_.data + joint_velocities_.data*dt;
                    
                } 
                // computation of joint efforts
                else if(cmd_interface_ == "effort"){
                    
                    // Desired twists as Eigen::VectorXd objects
                    Vector6d cartvel; cartvel << p.vel, des_vel_rot_;
                    Vector6d cartacc; cartacc << p.acc, des_acc_rot_;
                    
                    // Desired frame position and twists as KDL objects
                    KDL::Frame d_pos; d_pos.M = des_pos_rot_.M; d_pos.p = toKDL(p.pos);
                    KDL::Twist d_vel = toKDLTwist(cartvel);
                    KDL::Twist d_acc = toKDLTwist(cartacc);
                    
                    
                    if(cont_type_ == "jnt"){
                    
                    // From the trajectory in op. space to the trajectory in joint space thanks to CLIK algorithm
                    controller_->CLIK(d_pos, d_vel, d_acc, KP_clik, KD_clik, dpos, dvel, dacc, dt, *robot_, lambda_clik);
                    // Inverse Dynamics controller in joint space
                    joint_efforts_.data = controller_->idCntr(dpos, dvel, dacc, KP_j, KD_j, *robot_) - robot_->getGravity();
                    }
                    else if(cont_type_ == "op"){
                    // Inverse Dynamics controller in operational space
                    joint_efforts_.data = controller_->idCntr(d_pos, d_vel, d_acc, KP_o, KD_o, *robot_, lambda_op) - robot_->getGravity();
                    // Getting the final reference for ending pose
                    robot_->getInverseKinematics(d_pos, dpos);
                    }
                }   

                // Update KDLrobot structure
                robot_->update(toStdVector(joint_positions_.data),toStdVector(joint_velocities_.data));

                if(cmd_interface_ == "velocity"){
                    // Send joint velocity commands
                    for (long int i = 0; i < joint_velocities_.data.size(); ++i) {
                        desired_commands_[i] = joint_velocities_(i);
                    }
                } else if(cmd_interface_ == "effort"){
                    // Send joint effort commands
                    for (long int i = 0; i < joint_efforts_.data.size(); ++i) {
                        desired_commands_[i] = joint_efforts_(i);
                    }
                }

                // Create msg and publish
                std_msgs::msg::Float64MultiArray cmd_msg;
                cmd_msg.data = desired_commands_;
                cmdPublisher_->publish(cmd_msg);
            }
            
            
            
            */
            
            
            
            
            
            // Vision Task: Track Orientation
            
            if(task_ == "look_at_point"){
                
                get_tf(camera_frame, "world", "stereo_gazebo_left_camera_optical_frame");
                
                KDL::Frame ee_t0_frame;
                ee_t0_frame = cartpos.Inverse() * tce_frame;
                camera_frame = ee_t0_frame * camera_frame;
                
                double k = 1.0;
                
                KDL::Frame diff_frame;
                get_tf(diff_frame, "stereo_gazebo_left_camera_optical_frame", "aruco_marker_frame");
                KDL::Vector diff_position = diff_frame.p;
                Eigen::Vector3d cPo(diff_position.x(), diff_position.y(), diff_position.z());
                
                Eigen::Vector3d sd(0.0, 0.0, 1.0);
                Eigen::Vector3d s(cPo(0)/cPo.norm(), cPo(1)/cPo.norm(), cPo(2)/cPo.norm());
                Eigen::Matrix3d Ss = skew(s);
                
                Eigen::Matrix3d Rc = toEigen(camera_frame.M);
                Eigen::MatrixXd R(6, 6);
                R.setZero();
                R.block<3, 3>(0, 0) = Rc;
                R.block<3, 3>(3, 3) = Rc;
                
                Eigen::MatrixXd Tce(6, 6);
                Tce.setZero();
                get_tf(tce_frame, "tool0", "stereo_gazebo_left_camera_optical_frame");
                Eigen::Matrix3d TceRc = toEigen(tce_frame.M);
                Tce.block<3, 3>(0, 0) = TceRc;
                Tce.block<3, 3>(3, 3) = TceRc;
                
                Eigen::MatrixXd Jc = Tce * robot_->getEEJacobian().data;
                
                Eigen::MatrixXd L(3, 6);
                L.block<3, 3>(0, 0) = (-1/cPo.norm())*(Eigen::Matrix3d::Identity() - s*s.transpose());
                L.block<3, 3>(0, 3) = Ss;
                L=L*R;
                
                Eigen::MatrixXd N = Eigen::MatrixXd::Identity(robot_->getNrJnts(), robot_->getNrJnts()) - pseudoinverse(L*Jc) * L*Jc;
                
                if(traj_type_ == "no_traj"){
                    dvel.data = Eigen::VectorXd::Zero(robot_->getNrJnts());
                }
                
                dvel_vis.data = k * pseudoinverse(L*Jc) * sd;// + N * dvel.data;
                
                if(cmd_interface_ == "velocity"){
                    
                    joint_velocities_.data = dvel_vis.data;
                    joint_positions_.data = joint_positions_.data + joint_velocities_.data*dt;
                    
                }else if(cmd_interface_ == "effort"){
                    
                    dacc_vis.data = Eigen::VectorXd::Zero(robot_->getNrJnts());
                    dpos_vis.data = dpos_vis.data + dvel_vis.data*dt; 
                    joint_efforts_.data = controller_->idCntr(dpos_vis, dvel_vis, dacc_vis, KP_j, KD_j, *robot_) - robot_->getGravity();
                }
            }
            
            // Update KDLrobot structure
            robot_->update(toStdVector(joint_positions_.data),toStdVector(joint_velocities_.data));
            if(cmd_interface_ == "velocity"){
                // Send joint velocity commands
                for (long int i = 0; i < joint_velocities_.data.size(); ++i) {
                    desired_commands_[i] = joint_velocities_(i);
                }
            } else if(cmd_interface_ == "effort"){
                // Send joint effort commands
                for (long int i = 0; i < joint_efforts_.data.size(); ++i) {
                    desired_commands_[i] = joint_efforts_(i);
                }
            }
            // Create msg and publish
            std_msgs::msg::Float64MultiArray cmd_msg;
            cmd_msg.data = desired_commands_;
            cmdPublisher_->publish(cmd_msg);
            
            /*
            if (t_ < total_time){

                // Retrieve the trajectory point
                
                trajectory_point p;
                if(traj_type_ == "lin_pol"){
                    p = planner_linear.compute_trajectory_linear(t_);
                }else if(traj_type_ == "lin_trap"){
                    p = planner_linear.compute_trajectory_linear(t_, acc_duration);
                }else if(traj_type_ == "cir_pol"){
                    p = planner_circle.compute_trajectory_circle(t_);
                }else if(traj_type_ == "cir_trap"){
                    p = planner_circle.compute_trajectory_circle(t_, acc_duration);
                }
                
                KDL::Frame des_pos_rot_; des_pos_rot_.M = init_cart_pose_.M;
                Eigen::VectorXd des_vel_rot_ = Eigen::VectorXd::Zero(3);
                Eigen::VectorXd des_acc_rot_ = Eigen::VectorXd::Zero(3);

                // Compute EE frame
                KDL::Frame cartpos = robot_->getEEFrame();          

                // Compute desired Frame
                KDL::Frame desFrame; desFrame.M = cartpos.M; desFrame.p = toKDL(p.pos);
                
                // compute errors
                Eigen::Vector3d error = computeLinearError(p.pos, Eigen::Vector3d(cartpos.p.data));
                Eigen::Vector3d o_error = computeOrientationError(toEigen(init_cart_pose_.M), toEigen(cartpos.M));
                std::cout << "The error norm is : " << error.norm() << std::endl;

                if(cmd_interface_ == "velocity"){

                    // Compute differential IK
                    Vector6d cartvel; cartvel << p.vel + 5*error, o_error;
                    joint_velocities_.data = pseudoinverse(robot_->getEEJacobian().data)*cartvel;
                    joint_positions_.data = joint_positions_.data + joint_velocities_.data*dt;
                    
                } 
                // computation of joint efforts
                else if(cmd_interface_ == "effort"){
                    
                    // Desired twists as Eigen::VectorXd objects
                    Vector6d cartvel; cartvel << p.vel, des_vel_rot_;
                    Vector6d cartacc; cartacc << p.acc, des_acc_rot_;
                    
                    // Desired frame position and twists as KDL objects
                    KDL::Frame d_pos; d_pos.M = des_pos_rot_.M; d_pos.p = toKDL(p.pos);
                    KDL::Twist d_vel = toKDLTwist(cartvel);
                    KDL::Twist d_acc = toKDLTwist(cartacc);
                    
                    
                    if(cont_type_ == "jnt"){
                    
                    // From the trajectory in op. space to the trajectory in joint space thanks to CLIK algorithm
                    controller_->CLIK(d_pos, d_vel, d_acc, KP_clik, KD_clik, dpos, dvel, dacc, dt, *robot_, lambda_clik);
                    // Inverse Dynamics controller in joint space
                    joint_efforts_.data = controller_->idCntr(dpos, dvel, dacc, KP_j, KD_j, *robot_) - robot_->getGravity();
                    }
                    else if(cont_type_ == "op"){
                    // Inverse Dynamics controller in operational space
                    joint_efforts_.data = controller_->idCntr(d_pos, d_vel, d_acc, KP_o, KD_o, *robot_, lambda_op) - robot_->getGravity();
                    // Getting the final reference for ending pose
                    robot_->getInverseKinematics(d_pos, dpos);
                    }
                }   

                // Update KDLrobot structure
                robot_->update(toStdVector(joint_positions_.data),toStdVector(joint_velocities_.data));

                if(cmd_interface_ == "velocity"){
                    // Send joint velocity commands
                    for (long int i = 0; i < joint_velocities_.data.size(); ++i) {
                        desired_commands_[i] = joint_velocities_(i);
                    }
                } else if(cmd_interface_ == "effort"){
                    // Send joint effort commands
                    for (long int i = 0; i < joint_efforts_.data.size(); ++i) {
                        desired_commands_[i] = joint_efforts_(i);
                    }
                }

                // Create msg and publish
                std_msgs::msg::Float64MultiArray cmd_msg;
                cmd_msg.data = desired_commands_;
                cmdPublisher_->publish(cmd_msg);
            }
            else{
                RCLCPP_INFO_ONCE(this->get_logger(), "Trajectory executed successfully ...");
                // End Commands
                
                if(cmd_interface_ != "effort"){
                for (long int i = 0; i < joint_velocities_.data.size(); ++i) {
                    desired_commands_[i] = 0.0;
                }} else if(cmd_interface_ == "effort"){
                    
                    // Ending pose reference
                    robot_->update(toStdVector(joint_positions_.data),toStdVector(joint_velocities_.data));
                    dvel.data = Eigen::VectorXd::Zero(7);
                    dacc.data = Eigen::VectorXd::Zero(7);
                    
                    // Regulation to the ending pose
                    joint_efforts_.data = controller_->idCntr(dpos, dvel, dacc, KP_j, KD_j, *robot_) - robot_->getGravity();
                    for (long int i = 0; i < joint_efforts_.data.size(); ++i) {
                    desired_commands_[i] = joint_efforts_(i);
                    }
                }
                
                // Create msg and publish
                std_msgs::msg::Float64MultiArray cmd_msg;
                cmd_msg.data = desired_commands_;
                cmdPublisher_->publish(cmd_msg);
            }*/
        }

        void joint_state_subscriber(const sensor_msgs::msg::JointState& sensor_msg){
        
            joint_state_available_ = true;
            for (unsigned int i  = 0; i < sensor_msg.position.size(); i++){
                joint_positions_.data[i] = sensor_msg.position[i];
                joint_velocities_.data[i] = sensor_msg.velocity[i];
                joint_efforts_.data[i] = sensor_msg.effort[i];
            }
        }

        rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr jointSubscriber_;
        rclcpp::Publisher<FloatArray>::SharedPtr cmdPublisher_;
        rclcpp::TimerBase::SharedPtr timer_; 
        rclcpp::TimerBase::SharedPtr subTimer_;
        rclcpp::Node::SharedPtr node_handle_;

        std::vector<double> desired_commands_ = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        KDL::JntArray joint_positions_;
        KDL::JntArray joint_velocities_;
        KDL::JntArray joint_accelerations_;
        KDL::JntArray joint_efforts_;
        std::shared_ptr<KDLRobot> robot_;
        std::shared_ptr<KDLController> controller_;
        KDLPlanner planner_linear;
        KDLPlanner planner_circle;
        KDL::JntArray dpos, dvel, dacc;

        int iteration_;
        bool joint_state_available_;
        double t_;
        std::string cmd_interface_;
        std::string traj_type_;
        std::string cont_type_;
        std::string task_;
        KDL::Frame init_cart_pose_;
        
        double KP_j = 12;
        double KD_j = 5;
        double KP_clik = 10;
        double KD_clik = 4;
        double lambda_clik = 0.01;
        double KP_o = 8;
        double KD_o = 5;
        double lambda_op = 0.01;
        
        unsigned int freq_ms = 10;
        
        KDL::Frame aruco_frame;
        KDL::Frame camera_frame;
        std::shared_ptr<tf2_ros::Buffer> tf_buffer_;
        std::shared_ptr<tf2_ros::TransformListener> tf_listener_;
        
        double positioning_offset = 0.5;
        KDL::Frame tce_frame;
        bool flag_tce = 0;
        
        KDL::JntArray dpos_vis, dvel_vis, dacc_vis;
};

 
int main( int argc, char** argv )
{
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<Iiwa_pub_sub>());
    rclcpp::shutdown();
    return 1;
}
