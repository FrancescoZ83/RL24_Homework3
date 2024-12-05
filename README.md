# Vision Task Control

## :hammer: Setup

To install all the required packages run from the ros2 workspace:
```sh
source ./src/setup.sh
```
If instead the requested libraries are already insalled, just run:
```sh
colcon build
source install/setup.sh
```

## 🚴‍♂️ Blue Sphere Detection

In order to run the gazebo simulation environment, launch this command. `use_vision` loads the camera and `world` loads the world with the blue sphere
```sh
ros2 launch iiwa_bringup iiwa.launch.py use_sim:="true" use_vision:=true world:=src/ros2_iiwa/iiwa_description/gazebo/worlds/sphere.world
```

To run the detector node launch this on a second terminal
```
ros2 run ros2_opencv ros2_opencv_node
```
Note: the manipulator is loaded already pointing to the sphere

Use rqt to see the Images topics (/videocamera for the base camera and /processed_image for the detection node)
```
ros2 run rqt_image_view rqt_image_view
```




## 🦾 Executing the control task

To launch the simulation use the ad-hoc launcher
1. **Velocity Interface:**
```sh
ros2 launch ros2_kdl_package my_aruco_gazebo.launch.py
```
2. **Effort Interface:**
```sh
ros2 launch ros2_kdl_package my_aruco_gazebo.launch.py command_interface:="effort" robot_controller:="effort_controller"
```


As for the control node, it can accept different parameters from the shell to choose the command interface, the trajectory type and (in case of effort controller) the control type.
On a second terminal, the base command is the following (remember to add '-p' before each parameter)
```sh
ros2 run ros2_kdl_package ros2_kdl_node --ros-args
```
To set the parameters:
- `cmd_interface` - can be set to `position`, `velocity`, `effort`; To choose the command interface
- `traj_type` - can be set to `no_traj`, `lin_pol`, `lin_trap`, `cir_pol`, `cir_trap`; To choose the trajectory type (linear or circular) and the s computation (polinomial or trapezoidal)
- `cont_type` - can be set to `jnt`, `op`; To choose the control type (joint or operational space)
- `task` - can be set to `positioning` or `look_at_point` to select the vision task
- `q0_task` - can be set to `exploit` or `not_exploit` if you want to compute the orientation exploiting "N*q0_dot" or compute the trajectories in the regular way

### Examples

We used these examples to produce the sent videos (Be sure the aruco tag is seen from the robot at the beginning)

Positioning task
```sh
ros2 run ros2_kdl_package ros2_kdl_node --ros-args -p cmd_interface:=velocity -p task:=positioning
```

Look at point task with no trajectory
```sh
ros2 run ros2_kdl_package ros2_kdl_node --ros-args -p cmd_interface:=velocity -p task:=look_at_point
```

Look at point with joint space effort control, linear polinomial trajectory
```sh
ros2 run ros2_kdl_package ros2_kdl_node --ros-args -p cmd_interface:=effort -p task:=look_at_point -p traj_type:=lin_pol -p q0_task:=not_exploit -p cont_type:=jnt
```

Look at point with operational space effort control, trapezoidal circular trajectory
```sh
ros2 run ros2_kdl_package ros2_kdl_node --ros-args -p cmd_interface:=effort -p task:=look_at_point -p traj_type:=lin_pol -p q0_task:=not_exploit -p cont_type:=op
```
