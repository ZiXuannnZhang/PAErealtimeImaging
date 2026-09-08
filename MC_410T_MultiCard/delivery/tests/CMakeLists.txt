cmake_minimum_required(VERSION 3.21)
project(diagnostic_recorder_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ring_block_assembler_test
    ring_block_assembler_test.cpp
    ../src/RingBlockAssembler.cpp)
target_include_directories(ring_block_assembler_test PRIVATE ../include)
target_compile_features(ring_block_assembler_test PRIVATE cxx_std_17)
enable_testing()
add_test(NAME ring_block_assembler_test COMMAND ring_block_assembler_test)

find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Widgets)

add_executable(diagnostic_recorder_test
    diagnostic_recorder_test.cpp
    ../src/DiagnosticRecorder.cpp
)
target_include_directories(diagnostic_recorder_test PRIVATE ../include)
target_link_libraries(diagnostic_recorder_test PRIVATE Qt6::Core)

add_test(NAME diagnostic_recorder_test COMMAND diagnostic_recorder_test)

add_executable(ring_shm_observability_test ring_shm_observability_test.cpp)
target_include_directories(ring_shm_observability_test PRIVATE ../include)
target_link_libraries(ring_shm_observability_test PRIVATE Qt6::Core)
add_test(NAME ring_shm_observability_test COMMAND ring_shm_observability_test)

add_executable(diagnostic_dialog_test diagnostic_dialog_test.cpp
    ../src/DiagnosticExportDialog.cpp ../src/DiagnosticRecorder.cpp)
target_include_directories(diagnostic_dialog_test PRIVATE ../include)
target_compile_definitions(diagnostic_dialog_test PRIVATE DIAGNOSTIC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(diagnostic_dialog_test PRIVATE Qt6::Widgets Qt6::Concurrent)
add_test(NAME diagnostic_dialog_test COMMAND diagnostic_dialog_test)
set_tests_properties(diagnostic_dialog_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(WIN32)
    add_executable(process_scheduling_test
        process_scheduling_test.cpp
        ../src/ImagingSvc/ProcessScheduling.cpp)
    target_include_directories(process_scheduling_test PRIVATE
        ../include ../src/ImagingSvc)
    target_compile_definitions(process_scheduling_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(process_scheduling_test PRIVATE Qt6::Core)
    add_test(NAME process_scheduling_test COMMAND process_scheduling_test)

    add_executable(network_ingress_observability_test
        network_ingress_observability_test.cpp
        ../src/NetworkDiagnostics.cpp)
    target_include_directories(network_ingress_observability_test PRIVATE ../include)
    target_compile_definitions(network_ingress_observability_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(network_ingress_observability_test PRIVATE Qt6::Core ws2_32 iphlpapi)
    add_test(NAME network_ingress_observability_test COMMAND network_ingress_observability_test)

    add_executable(network_snapshot_test network_snapshot_test.cpp ../src/NetworkDiagnostics.cpp)
    target_include_directories(network_snapshot_test PRIVATE ../include)
    target_compile_definitions(network_snapshot_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(network_snapshot_test PRIVATE Qt6::Core ws2_32 iphlpapi)
    add_test(NAME network_snapshot_test COMMAND network_snapshot_test)
    set(CMAKE_AUTOMOC ON)
    add_executable(network_diagnostics_test
        network_diagnostics_test.cpp
        ../src/NetworkController.cpp ../include/NetworkController.h
        ../src/NetworkDiagnostics.cpp ../src/DiagnosticRecorder.cpp
        ../src/DataProcessor.cpp ../include/DataProcessor.h
        ../src/FileSaver.cpp ../include/FileSaver.h
        ../src/MultiPortReceiver.cpp ../include/MultiPortReceiver.h
        ../src/FramePublisher.cpp ../include/FramePublisher.h)
    target_include_directories(network_diagnostics_test PRIVATE ../include)
    target_compile_definitions(network_diagnostics_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(network_diagnostics_test PRIVATE Qt6::Core Qt6::Concurrent ws2_32 iphlpapi)
    add_test(NAME network_diagnostics_test COMMAND network_diagnostics_test)

    add_executable(data_processor_batch_test
        data_processor_batch_test.cpp
        ../src/DataProcessor.cpp
        ../include/DataProcessor.h
        ../src/FramePublisher.cpp
        ../include/FramePublisher.h
        ../src/MultiPortReceiver.cpp
        ../include/MultiPortReceiver.h)
    target_include_directories(data_processor_batch_test PRIVATE ../include)
    target_compile_definitions(data_processor_batch_test PRIVATE
        DATA_PROCESSOR_TEST_SEAM WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(data_processor_batch_test PRIVATE Qt6::Core ws2_32)
    add_test(NAME data_processor_batch_test COMMAND data_processor_batch_test)

    add_executable(card_status_formatting_test card_status_formatting_test.cpp)
    target_include_directories(card_status_formatting_test PRIVATE ../include)
    target_link_libraries(card_status_formatting_test PRIVATE Qt6::Core)
    add_test(NAME card_status_formatting_test COMMAND card_status_formatting_test)
endif()
