cmake_minimum_required(VERSION 3.16)
project(rocos_igh VERSION 0.1.0 LANGUAGES CXX)

option(ROCOS_IGH_BUILD_MASTER "Build the IgH master executable" ON)
option(ROCOS_IGH_BUILD_HARDWARE_TESTS "Enable tests requiring EtherCAT hardware" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(Threads REQUIRED)

add_library(rocos_igh_core INTERFACE)
target_include_directories(rocos_igh_core INTERFACE
	"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_compile_features(rocos_igh_core INTERFACE cxx_std_17)
target_link_libraries(rocos_igh_core INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_link_libraries(rocos_igh_core INTERFACE rt)
endif()

if(ROCOS_IGH_BUILD_MASTER)
	find_package(EtherCAT REQUIRED)
endif()

include(CTest)
if(BUILD_TESTING)
	add_executable(shared_memory_config_test tests/shared_memory_config_test.cpp)
	target_link_libraries(shared_memory_config_test PRIVATE rocos_igh_core)
	add_test(NAME shared_memory_config COMMAND shared_memory_config_test)
endif()

