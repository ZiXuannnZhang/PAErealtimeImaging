cmake_minimum_required(VERSION 3.21)
project(diagnostic_recorder_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Widgets)

add_executable(diagnostic_recorder_test
    diagnostic_recorder_test.cpp
    ../src/DiagnosticRecorder.cpp
)
target_include_directories(diagnostic_recorder_test PRIVATE ../include)
target_link_libraries(diagnostic_recorder_test PRIVATE Qt6::Core)

enable_testing()
add_test(NAME diagnostic_recorder_test COMMAND diagnostic_recorder_test)

add_executable(diagnostic_dialog_test diagnostic_dialog_test.cpp
    ../src/DiagnosticExportDialog.cpp ../src/DiagnosticRecorder.cpp)
target_include_directories(diagnostic_dialog_test PRIVATE ../include)
target_compile_definitions(diagnostic_dialog_test PRIVATE DIAGNOSTIC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(diagnostic_dialog_test PRIVATE Qt6::Widgets Qt6::Concurrent)
add_test(NAME diagnostic_dialog_test COMMAND diagnostic_dialog_test)
set_tests_properties(diagnostic_dialog_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(WIN32)
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
endif()
