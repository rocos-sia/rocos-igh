cmake_minimum_required(VERSION 3.16)
project(rocos_igh VERSION 0.1.0 LANGUAGES CXX)

option(ROCOS_IGH_BUILD_MASTER "Build the IgH master executable" ON)  # default ON: requires ecrt.h and libethercat
option(ROCOS_IGH_BUILD_HARDWARE_TESTS "Enable tests requiring EtherCAT hardware" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(Threads REQUIRED)
find_package(yaml-cpp REQUIRED)

if(ROCOS_IGH_BUILD_MASTER)
	find_package(EtherCAT REQUIRED)
	add_library(rocos_igh_core STATIC
		src/pdo_config.cpp
		src/pdo_verification.cpp
		src/slave_config.cpp
		src/ethercat_master.cpp
		src/cyclic_task.cpp
		src/runtime_options.cpp
		src/thread_affinity.cpp
	)
	target_include_directories(rocos_igh_core PUBLIC
		"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
	)
	target_compile_features(rocos_igh_core PUBLIC cxx_std_17)
	target_compile_definitions(rocos_igh_core PUBLIC ROCOS_IGH_BUILD_MASTER=1)
	target_link_libraries(rocos_igh_core PUBLIC
		EtherCAT::EtherCAT
		yaml-cpp
		Threads::Threads
		${CMAKE_DL_LIBS}
	)
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		target_link_libraries(rocos_igh_core PUBLIC rt)
	endif()

	add_executable(rocos_igh_master src/main.cpp)
	target_link_libraries(rocos_igh_master PRIVATE rocos_igh_core)
else()
	add_library(rocos_igh_core INTERFACE)
	target_include_directories(rocos_igh_core INTERFACE
		"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
	)
	target_compile_features(rocos_igh_core INTERFACE cxx_std_17)
	target_compile_definitions(rocos_igh_core INTERFACE ROCOS_IGH_BUILD_MASTER=0)
	target_link_libraries(rocos_igh_core INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		target_link_libraries(rocos_igh_core INTERFACE rt)
	endif()
endif()

include(CTest)
if(BUILD_TESTING)
	add_executable(runtime_report_test tests/runtime_report_test.cpp)
	target_include_directories(runtime_report_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
	target_compile_features(runtime_report_test PRIVATE cxx_std_17)
	target_link_libraries(runtime_report_test PRIVATE Threads::Threads)
	add_test(NAME runtime_report COMMAND runtime_report_test)
	set_tests_properties(runtime_report PROPERTIES TIMEOUT 15)
	add_executable(runtime_options_test tests/runtime_options_test.cpp src/runtime_options.cpp)
	target_include_directories(runtime_options_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
	target_compile_features(runtime_options_test PRIVATE cxx_std_17)
	add_test(NAME runtime_options COMMAND runtime_options_test)
	add_executable(thread_affinity_test tests/thread_affinity_test.cpp src/thread_affinity.cpp)
	target_include_directories(thread_affinity_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
	target_compile_features(thread_affinity_test PRIVATE cxx_std_17)
	target_link_libraries(thread_affinity_test PRIVATE Threads::Threads)
	add_test(NAME thread_affinity COMMAND thread_affinity_test)
	set_tests_properties(thread_affinity PROPERTIES TIMEOUT 10)
	if(ROCOS_IGH_BUILD_MASTER)
		add_executable(pdo_verification_test tests/pdo_verification_test.cpp)
		target_link_libraries(pdo_verification_test PRIVATE rocos_igh_core)
		foreach(symbol ecrt_slave_config_create_sdo_request ecrt_sdo_request_timeout
			ecrt_sdo_request_index ecrt_sdo_request_read ecrt_sdo_request_state
			ecrt_sdo_request_data_size ecrt_sdo_request_data clock_gettime clock_nanosleep
			ecrt_master_receive ecrt_domain_process ecrt_master_state ecrt_domain_state
			ecrt_slave_config_state ecrt_domain_queue ecrt_master_send ecrt_release_master
			ecrt_master_get_slave ecrt_master_sdo_download ecrt_master_sdo_upload)
			target_link_options(pdo_verification_test PRIVATE "-Wl,--wrap=${symbol}")
		endforeach()
		add_test(NAME pdo_verification COMMAND pdo_verification_test)
		set_tests_properties(pdo_verification PROPERTIES TIMEOUT 15)
		add_executable(startup_state_test tests/startup_state_test.cpp)
		target_link_libraries(startup_state_test PRIVATE rocos_igh_core)
		target_link_options(startup_state_test PRIVATE "-Wl,--wrap=ecrt_slave_config_state")
		add_test(NAME startup_state COMMAND startup_state_test)
		add_executable(dc_timing_test tests/dc_timing_test.cpp)
		target_compile_definitions(dc_timing_test PRIVATE ROCOS_IGH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
		target_link_libraries(dc_timing_test PRIVATE rocos_igh_core)
		foreach(symbol clock_gettime ecrt_master_activate ecrt_master_application_time
			ecrt_master_sync_reference_clock_to ecrt_master_sync_slave_clocks
			ecrt_domain_queue ecrt_master_send ecrt_release_master)
			target_link_options(dc_timing_test PRIVATE "-Wl,--wrap=${symbol}")
		endforeach()
		add_test(NAME dc_timing COMMAND dc_timing_test)
	endif()
	add_executable(pdo_config_test tests/pdo_config_test.cpp src/pdo_config.cpp src/runtime_options.cpp)
	target_include_directories(pdo_config_test PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_compile_definitions(pdo_config_test PRIVATE
		ROCOS_IGH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
	)
	target_compile_features(pdo_config_test PRIVATE cxx_std_17)
	target_link_libraries(pdo_config_test PRIVATE yaml-cpp)
	add_test(NAME pdo_config COMMAND pdo_config_test)

	add_executable(shared_memory_config_test tests/shared_memory_config_test.cpp)
	if(NOT ROCOS_IGH_BUILD_MASTER)
		target_sources(shared_memory_config_test PRIVATE src/cyclic_task.cpp src/runtime_options.cpp)
	endif()
	target_compile_definitions(shared_memory_config_test PRIVATE
		ROCOS_IGH_TESTING=1
		ROCOS_IGH_BUILD_MASTER=$<BOOL:${ROCOS_IGH_BUILD_MASTER}>
	)
	target_link_libraries(shared_memory_config_test PRIVATE rocos_igh_core)
	add_test(NAME shared_memory_config COMMAND shared_memory_config_test)
endif()
