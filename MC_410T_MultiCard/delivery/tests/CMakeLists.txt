cmake_minimum_required(VERSION 3.21)
project(diagnostic_recorder_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Python3 COMPONENTS Interpreter QUIET)

add_executable(ring_block_assembler_test
    ring_block_assembler_test.cpp
    ../src/RingBlockAssembler.cpp)
target_include_directories(ring_block_assembler_test PRIVATE ../include)
target_compile_features(ring_block_assembler_test PRIVATE cxx_std_17)
enable_testing()
add_executable(timeout_presentation_test timeout_presentation_test.cpp ../src/RingRoundUiState.cpp)
target_include_directories(timeout_presentation_test PRIVATE ../include)
target_link_libraries(timeout_presentation_test PRIVATE Qt6::Core)
add_test(NAME timeout_presentation_test COMMAND timeout_presentation_test)
add_test(NAME ring_block_assembler_test COMMAND ring_block_assembler_test)

add_executable(imaging_bypass_test imaging_bypass_test.cpp ../src/ImagingBypass.cpp)
target_include_directories(imaging_bypass_test PRIVATE ../include)
target_compile_features(imaging_bypass_test PRIVATE cxx_std_17)
add_test(NAME imaging_bypass_test COMMAND imaging_bypass_test)

add_executable(data_processor_imaging_isolation_test
    data_processor_imaging_isolation_test.cpp
    ../src/ImagingBypass.cpp
    ../src/FrontendPreprocessor.cpp ../include/FrontendPreprocessor.h ../src/FrontendFilter.cpp ../include/FrontendFilter.h
    ../src/DataProcessor.cpp ../include/DataProcessor.h
    ../src/FramePublisher.cpp ../include/FramePublisher.h)
target_include_directories(data_processor_imaging_isolation_test PRIVATE ../include)
target_compile_definitions(data_processor_imaging_isolation_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(data_processor_imaging_isolation_test PRIVATE Qt6::Core)
add_test(NAME data_processor_imaging_isolation_test COMMAND data_processor_imaging_isolation_test)

# Frontend Preprocessing Stage (Task 1 identity processing) contract tests.
add_executable(frontend_preprocessor_test
    frontend_preprocessor_test.cpp
    ../src/FrontendPreprocessor.cpp ../include/FrontendPreprocessor.h ../src/FrontendFilter.cpp ../include/FrontendFilter.h
    ../src/ImagingBypass.cpp
    ../src/DataProcessor.cpp ../include/DataProcessor.h
    ../src/FramePublisher.cpp ../include/FramePublisher.h)
target_include_directories(frontend_preprocessor_test PRIVATE ../include)
target_compile_definitions(frontend_preprocessor_test PRIVATE
    FRONTEND_PREPROCESSOR_TEST_SEAM WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(frontend_preprocessor_test PRIVATE Qt6::Core)
add_test(NAME frontend_preprocessor_test COMMAND frontend_preprocessor_test)

find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Widgets)

add_executable(ring_production_blockers_test ring_production_blockers_test.cpp
    ../src/RingBlockAssembler.cpp ../src/RingRoundUiState.cpp ../src/RingRoundPresentation.cpp
    ../src/PaimageAcquisition/PhysicalRoundNormalizer.cpp)
target_include_directories(ring_production_blockers_test PRIVATE ../include)
target_link_libraries(ring_production_blockers_test PRIVATE Qt6::Core)
add_test(NAME ring_production_blockers_test COMMAND ring_production_blockers_test)

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
if(WIN32 AND TARGET Qt6::QOffscreenIntegrationPlugin)
    add_custom_command(TARGET diagnostic_dialog_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:diagnostic_dialog_test>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
            "$<TARGET_FILE_DIR:diagnostic_dialog_test>/platforms/")
endif()

add_executable(diagnostic_time_window_test
    diagnostic_time_window_test.cpp
    ../src/DiagnosticRecorder.cpp)
target_include_directories(diagnostic_time_window_test PRIVATE ../include)
target_link_libraries(diagnostic_time_window_test PRIVATE Qt6::Core)
add_test(NAME diagnostic_time_window_test COMMAND diagnostic_time_window_test)

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
        ../src/PaimageAcquisition/AutoSaveRoundCoordinator.cpp
        ../include/PaimageAcquisition/AutoSaveRoundCoordinator.h
        ../src/MeasurementSession.cpp ../include/MeasurementSession.h
        ../src/NetworkDiagnostics.cpp ../src/DiagnosticRecorder.cpp
        ../src/StartupPolicy.cpp
        ../src/DataProcessor.cpp ../include/DataProcessor.h
        ../src/FrontendPreprocessor.cpp ../include/FrontendPreprocessor.h ../src/FrontendFilter.cpp ../include/FrontendFilter.h
        ../src/FileSaver.cpp ../include/FileSaver.h
        ../src/MultiPortReceiver.cpp ../include/MultiPortReceiver.h
        ../src/FramePublisher.cpp ../include/FramePublisher.h)
    target_include_directories(network_diagnostics_test PRIVATE ../include)
    target_compile_definitions(network_diagnostics_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(network_diagnostics_test PRIVATE Qt6::Core Qt6::Concurrent ws2_32 iphlpapi)
    # The removed production StartFence is not a PAimage acceptance criterion.
    # Retain historical control/field helpers; real source admission/export is
    # exercised by paimage_network_test and paimage_trace_bundle_test.
    add_test(NAME network_diagnostics_test COMMAND network_diagnostics_test --legacy-control-only)

    add_executable(measurement_session_transaction_test
        measurement_session_transaction_test.cpp
        ../src/MeasurementSession.cpp)
    target_include_directories(measurement_session_transaction_test PRIVATE ../include)
    target_link_libraries(measurement_session_transaction_test PRIVATE Qt6::Core)
    add_test(NAME measurement_session_transaction_test COMMAND measurement_session_transaction_test)

    add_executable(session_boundary_test
        session_boundary_test.cpp
        ../src/DataProcessor.cpp
        ../include/DataProcessor.h
        ../src/FramePublisher.cpp
        ../include/FramePublisher.h
        ../src/MultiPortReceiver.cpp
        ../include/MultiPortReceiver.h)
    target_include_directories(session_boundary_test PRIVATE ../include)
    target_compile_definitions(session_boundary_test PRIVATE
        DATA_PROCESSOR_TEST_SEAM WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(session_boundary_test PRIVATE Qt6::Core ws2_32)
    add_test(NAME session_boundary_test COMMAND session_boundary_test)

    add_executable(session_boundary_receiver_test
        session_boundary_receiver_test.cpp
        ../src/DataProcessor.cpp
        ../include/DataProcessor.h
        ../src/FramePublisher.cpp
        ../include/FramePublisher.h
        ../src/MultiPortReceiver.cpp
        ../include/MultiPortReceiver.h
        ../src/MeasurementSession.cpp
        ../include/MeasurementSession.h)
    target_include_directories(session_boundary_receiver_test PRIVATE ../include)
    target_compile_definitions(session_boundary_receiver_test PRIVATE
        MULTI_PORT_RECEIVER_TEST_SEAM WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(session_boundary_receiver_test PRIVATE Qt6::Core ws2_32)
    add_test(NAME session_boundary_receiver_test COMMAND session_boundary_receiver_test)

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
    # session_boundary_receiver_test and data_processor_batch_test both bind the
    # fixed production port BASE_PORT(8001) via MultiPortReceiver; a resource
    # lock keeps them from overlapping under ctest -j (pre-existing race).
    set_tests_properties(session_boundary_receiver_test data_processor_batch_test
        PROPERTIES RESOURCE_LOCK paimage_base_port_8001)

    add_executable(card_status_formatting_test card_status_formatting_test.cpp)
    target_include_directories(card_status_formatting_test PRIVATE ../include)
    target_link_libraries(card_status_formatting_test PRIVATE Qt6::Core)
    add_test(NAME card_status_formatting_test COMMAND card_status_formatting_test)
    add_executable(round_policy_settings_test round_policy_settings_test.cpp)
    target_include_directories(round_policy_settings_test PRIVATE ../include)
    target_link_libraries(round_policy_settings_test PRIVATE Qt6::Core)
    add_test(NAME round_policy_settings_test COMMAND round_policy_settings_test)
    add_subdirectory(paimage_core)
    target_sources(network_diagnostics_test PRIVATE
        ../src/PaimageAcquisition/HostOutput.cpp
        ../src/PaimageAcquisition/PhysicalRoundNormalizer.cpp
        ../src/PaimageAcquisition/Backend.cpp
        ../src/PaimageAcquisition/NetworkControllerPaimage.cpp)
    target_link_libraries(network_diagnostics_test PRIVATE paimage_socket)
    get_target_property(PAIMAGE_NETWORK_TEST_SOURCES network_diagnostics_test SOURCES)
    list(REMOVE_ITEM PAIMAGE_NETWORK_TEST_SOURCES network_diagnostics_test.cpp)
    add_executable(paimage_network_test paimage_network_test.cpp ${PAIMAGE_NETWORK_TEST_SOURCES})
    target_include_directories(paimage_network_test PRIVATE ../include)
    target_compile_definitions(paimage_network_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(paimage_network_test PRIVATE paimage_socket Qt6::Core Qt6::Concurrent ws2_32 iphlpapi)
    add_test(NAME paimage_network_test COMMAND paimage_network_test)
    set_tests_properties(paimage_network_test PROPERTIES RUN_SERIAL TRUE)
    # Session B review addendum: production-seam trigger-gap accounting proof.
    add_executable(paimage_production_gap_test paimage_production_gap_test.cpp ${PAIMAGE_NETWORK_TEST_SOURCES})
    target_include_directories(paimage_production_gap_test PRIVATE ../include)
    target_compile_definitions(paimage_production_gap_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(paimage_production_gap_test PRIVATE paimage_socket Qt6::Core Qt6::Concurrent ws2_32 iphlpapi)
    add_test(NAME paimage_production_gap_test COMMAND paimage_production_gap_test)
    set_tests_properties(paimage_production_gap_test PROPERTIES RUN_SERIAL TRUE)
    add_executable(paimage_start_race_test paimage_start_race_test.cpp)
    target_include_directories(paimage_start_race_test PRIVATE ../include)
    target_compile_definitions(paimage_start_race_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(paimage_start_race_test PRIVATE paimage_socket ws2_32)
    add_test(NAME paimage_start_race
        COMMAND paimage_start_race_test --output "${CMAKE_CURRENT_BINARY_DIR}/start-race-artifacts")
    set_tests_properties(paimage_start_race PROPERTIES RUN_SERIAL TRUE)
    add_executable(paimage_start_fence_regression_test paimage_start_fence_regression_test.cpp)
    target_include_directories(paimage_start_fence_regression_test PRIVATE ../include)
    target_compile_definitions(paimage_start_fence_regression_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(paimage_start_fence_regression_test PRIVATE paimage_socket ws2_32)
    add_test(NAME paimage_start_fence_regression COMMAND paimage_start_fence_regression_test)
    set_tests_properties(paimage_start_fence_regression PROPERTIES RUN_SERIAL TRUE)
    add_executable(paimage_start_overflow_test paimage_start_overflow_test.cpp)
    target_include_directories(paimage_start_overflow_test PRIVATE ../include)
    target_compile_definitions(paimage_start_overflow_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(paimage_start_overflow_test PRIVATE paimage_socket ws2_32)
    add_test(NAME paimage_start_overflow COMMAND paimage_start_overflow_test)
    set_tests_properties(paimage_start_overflow PROPERTIES RUN_SERIAL TRUE)
    add_executable(paimage_host_output_test paimage_host_output_test.cpp
        ../src/PaimageAcquisition/HostOutput.cpp
        ../src/PaimageAcquisition/PhysicalRoundNormalizer.cpp
        ../src/PaimageAcquisition/Backend.cpp
        ../src/DataProcessor.cpp ../include/DataProcessor.h
        ../src/FrontendPreprocessor.cpp ../include/FrontendPreprocessor.h ../src/FrontendFilter.cpp ../include/FrontendFilter.h
        ../src/FileSaver.cpp ../include/FileSaver.h
        ../src/FramePublisher.cpp ../include/FramePublisher.h)
    target_include_directories(paimage_host_output_test PRIVATE ../include)
    target_compile_definitions(paimage_host_output_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(paimage_host_output_test PRIVATE paimage_socket Qt6::Core)
    add_test(NAME paimage_host_output_test COMMAND paimage_host_output_test)
    add_executable(filesaver_round_boundary_test filesaver_round_boundary_test.cpp
        ../src/PaimageAcquisition/HostOutput.cpp
        ../src/PaimageAcquisition/PhysicalRoundNormalizer.cpp
        ../src/PaimageAcquisition/Backend.cpp
        ../src/DataProcessor.cpp ../include/DataProcessor.h
        ../src/FrontendPreprocessor.cpp ../include/FrontendPreprocessor.h ../src/FrontendFilter.cpp ../include/FrontendFilter.h
        ../src/FileSaver.cpp ../include/FileSaver.h
        ../src/FramePublisher.cpp ../include/FramePublisher.h)
    target_include_directories(filesaver_round_boundary_test PRIVATE ../include)
    target_compile_definitions(filesaver_round_boundary_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(filesaver_round_boundary_test PRIVATE paimage_socket Qt6::Core)
    add_test(NAME filesaver_round_boundary_test COMMAND filesaver_round_boundary_test)
    add_executable(ring_round_ui_state_test ring_round_ui_state_test.cpp
        ../src/RingRoundUiState.cpp ../include/RingRoundUiState.h)
    target_include_directories(ring_round_ui_state_test PRIVATE ../include)
    target_compile_features(ring_round_ui_state_test PRIVATE cxx_std_17)
    add_test(NAME ring_round_ui_state_test COMMAND ring_round_ui_state_test)
    add_executable(ring_round_identity_test ring_round_identity_test.cpp
        ../src/RingRoundPresentation.cpp ../src/RingRoundUiState.cpp)
    target_include_directories(ring_round_identity_test PRIVATE ../include)
    target_compile_features(ring_round_identity_test PRIVATE cxx_std_17)
    target_link_libraries(ring_round_identity_test PRIVATE Qt6::Core)
    add_test(NAME ring_round_identity_test COMMAND ring_round_identity_test)
    add_executable(auto_save_round_coordinator_test
        auto_save_round_coordinator_test.cpp
        ../src/PaimageAcquisition/AutoSaveRoundCoordinator.cpp
        ../include/PaimageAcquisition/AutoSaveRoundCoordinator.h
        ../src/FileSaver.cpp ../include/FileSaver.h)
    target_include_directories(auto_save_round_coordinator_test PRIVATE ../include)
    target_compile_definitions(auto_save_round_coordinator_test PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(auto_save_round_coordinator_test PRIVATE Qt6::Core)
    add_test(NAME auto_save_round_coordinator_test COMMAND auto_save_round_coordinator_test)
    add_executable(count_boundary_save_binding_test
        count_boundary_save_binding_test.cpp
        ../src/PaimageAcquisition/AutoSaveRoundCoordinator.cpp
        ../include/PaimageAcquisition/AutoSaveRoundCoordinator.h
        ../src/PaimageAcquisition/HostOutput.cpp
        ../src/PaimageAcquisition/PhysicalRoundNormalizer.cpp
        ../src/DataProcessor.cpp ../include/DataProcessor.h
        ../src/FrontendPreprocessor.cpp ../include/FrontendPreprocessor.h ../src/FrontendFilter.cpp ../include/FrontendFilter.h
        ../src/FileSaver.cpp ../include/FileSaver.h
        ../src/FramePublisher.cpp ../include/FramePublisher.h)
    target_include_directories(count_boundary_save_binding_test PRIVATE ../include)
    target_compile_definitions(count_boundary_save_binding_test PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(count_boundary_save_binding_test PRIVATE paimage_socket Qt6::Core Qt6::Gui)
    add_test(NAME count_boundary_save_binding_test COMMAND count_boundary_save_binding_test)
    add_executable(paimage_trace_bundle_test paimage_trace_bundle_test.cpp
        ../src/PaimageAcquisition/TraceBundle.cpp ../src/DiagnosticRecorder.cpp)
    target_include_directories(paimage_trace_bundle_test PRIVATE ../include)
    target_link_libraries(paimage_trace_bundle_test PRIVATE paimage_socket Qt6::Core)
    add_test(NAME paimage_trace_bundle_test COMMAND paimage_trace_bundle_test)
endif()

if(Python3_Interpreter_FOUND)
    add_test(NAME startup_diagnostics_analyze_test
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/startup_diagnostics_analyze_test.py"
                "${CMAKE_CURRENT_BINARY_DIR}/startup-diagnostics-fixtures")
endif()

if(WIN32 AND Python3_Interpreter_FOUND)
    add_test(NAME paimage_start_race_analyzer
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/paimage_start_race_analyzer_test.py"
                "${CMAKE_CURRENT_BINARY_DIR}/start-race-artifacts"
                "${CMAKE_CURRENT_SOURCE_DIR}/../tools/startup_diagnostics_analyze.py")
    set_tests_properties(paimage_start_race_analyzer PROPERTIES
        DEPENDS paimage_start_race
        RUN_SERIAL TRUE)
endif()
