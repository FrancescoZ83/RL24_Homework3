#!/bin/bash

sudo apt update
sudo apt-get install libeigen3-dev libcppunit-dev
rosdep update
rosdep install --from-paths src --ignore-src -r -y

sudo apt-get install ros-humble-usb-cam -y
sudo apt-get install ros-humble-image-pipeline -y
sudo apt-get install ros-humble-tf-transformations -y

colcon build
export GZ_SIM_RESOURCE_PATH=$GZ_SIM_RESOURCE_PATH:~/ros2_ws/src/ros2_iiwa/iiwa_description/gazebo/models
source install/setup.bash
