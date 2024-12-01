from launch import LaunchDescription
from launch.actions import IncludeLaunchDescription
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch_ros.actions import Node
from launch.substitutions import LaunchConfiguration
from launch_ros.substitutions import FindPackageShare

def generate_launch_description():
    # Define marker parameters
    marker_size = LaunchConfiguration('marker_size', default='0.1')
    marker_id = LaunchConfiguration('marker_id', default='201')

    # Path to the aruco_ros single.launch.py file
    aruco_launch_file = FindPackageShare('aruco_ros').find('aruco_ros') + '/launch/single.launch.py'

    # Include the single.launch.py with arguments
    aruco_single_launch = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(aruco_launch_file),
        launch_arguments={
            'marker_size': marker_size,
            'marker_id': marker_id,
        }.items(),
    )

    # Static transform publisher Node
    static_transform_publisher = Node(
        package='tf2_ros',
        executable='static_transform_publisher',
        name='static_transform_publisher',
        arguments=['0', '0', '0', '1.57', '3.14', '1.57', 'camera_link', 'stereo_gazebo_left_camera_optical_frame'],
        output='screen',
    )

    # rqt_image_view Node
    rqt_image_view = Node(
        package='rqt_image_view',
        executable='rqt_image_view',
        name='rqt_image_view',
        output='screen',
    )

    return LaunchDescription([
        aruco_single_launch,
        static_transform_publisher,
        rqt_image_view,
    ])

