# This is a throwaway CMake project. Most likely this'll all be plumbed in to the
# proper CMake files throughout the project
cmake_minimum_required(VERSION 3.16...3.25)
project(armsx2_switch_probe C CXX)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/cpp/cmake")

include(Pcsx2Utils)
detect_operating_system()
detect_compiler()

if(HORIZON)
	message(STATUS "PROBE: Horizon platform detected OK (compiler id: ${CMAKE_CXX_COMPILER_ID})")
else()
	message(FATAL_ERROR "PROBE: Horizon NOT detected (CMAKE_SYSTEM_NAME='${CMAKE_SYSTEM_NAME}')")
endif()
