cmake_minimum_required(VERSION 3.20)
project(wengine_msgs)

# IDL package for the WEngine ROS2 bridge (ROS2-2: control services;
# ROS2-3: SignalBlock state message + CLI wiring). Built only when the
# top-level BUILD_ROS2 option is ON (see ../../../CMakeLists.txt), so default
# builds never require a ROS install. Plain CMake is enough: the spike
# behind ROS2-2 proved configure + build + uninstalled runtime (service
# create/call via the build-tree ament index) with a sourced ROS env.

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "srv/SigCommand.srv"
  "msg/SignalBlock.msg"
)

# Build-tree C++ typesupport target for consumers (the `wengine_msgs::`
# namespaced targets only exist in the install export; inside one build
# tree the documented accessor is rosidl_get_typesupport_target).
rosidl_get_typesupport_target(wengine_msgs_cpp_typesupport ${PROJECT_NAME} "rosidl_typesupport_cpp")
set(WENGINE_MSGS_CPP_TYPESUPPORT "${wengine_msgs_cpp_typesupport}" PARENT_SCOPE)

ament_package()
