cmake_minimum_required(VERSION 3.21)
project(diagnostic_recorder_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Python3 COMPONENTS Interpreter QUIET)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent)

add_executable(ring_timevar_lowpass_test ring_timevar_lowpass_test.cpp
    ../src/RingTimevarLowpass.cpp ../src/RingTimevarLowpassSettings.cpp ../src/RingRecon/ring_recon.cpp)
target_include_directories(ring_timevar_lowpass_test PRIVATE ../include ../src/RingRecon)
target_link_libraries(ring_timevar_lowpass_test PRIVATE Qt6::Core)
target_compile_definitions(ring_timevar_lowpass_test PRIVATE TIMEVAR_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/reference/fixtures")
add_test(NAME ring_timevar_lowpass_test COMMAND ring_timevar_lowpass_test)

add_executable(ring_timevar_bench ring_timevar_bench.cpp ../src/RingTimevarLowpass.cpp ../src/RingRecon/ring_recon.cpp)
target_include_directories(ring_timevar_bench PRIVATE ../include ../src/RingRecon)

if(WIN32)
    add_executable(ring_timevar_service_test ring_timevar_service_test.cpp
        ../src/ImagingSvc/ImagingSvc.cpp ../src/ImagingSvc/ImagingSvc.h ../src/ImagingSvc/ProcessScheduling.cpp
        ../src/RingRecon/ring_recon.cpp ../src/RingTimevarLowpass.cpp ../src/RingTimevarLowpassSettings.cpp ../src/FilteredDasSettings.cpp ../src/FilteredDasPrefilter.cpp)
    target_include_directories(ring_timevar_service_test PRIVATE ../include ../src/RingRecon ../src/ImagingSvc
        ../libs/imaging ../third_party/zeromq/include)
    target_compile_definitions(ring_timevar_service_test PRIVATE RING_TIMEVAR_TEST_SEAM WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(ring_timevar_service_test PRIVATE Qt6::Core ws2_32
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../_migration_pack/prebuilt_cuda/bin/libring_recon_cuda.dll.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/zeromq/lib/libzmq.dll.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../libs/imaging/pa_recon_core.lib")
    add_custom_command(TARGET ring_timevar_service_test POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../_migration_pack/prebuilt_cuda/bin/ring_recon_cuda.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../_migration_pack/prebuilt_cuda/bin/cudart64_12.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/../libs/imaging/pa_recon_core.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/../libs/imaging/cufft64_12.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/zeromq/bin/libzmq-v141-mt-4_3_5.dll"
        "$<TARGET_FILE_DIR:ring_timevar_service_test>")
    add_test(NAME ring_timevar_service_test COMMAND ring_timevar_service_test)
    set_tests_properties(ring_timevar_service_test PROPERTIES RUN_SERIAL TRUE)
    add_executable(ring_timevar_ui_test ring_timevar_ui_test.cpp
        ../src/RingConfigDialog.cpp ../include/RingConfigDialog.h
        ../src/ImagingController.cpp ../include/ImagingController.h
        ../src/FrontendFilter.cpp ../src/RingTimevarLowpass.cpp ../src/RingTimevarLowpassSettings.cpp ../src/FilteredDasSettings.cpp ../src/FilteredDasPrefilter.cpp)
    target_include_directories(ring_timevar_ui_test PRIVATE ../include ../src/RingRecon
        ../third_party/zeromq/include)
    target_compile_definitions(ring_timevar_ui_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(ring_timevar_ui_test PRIVATE Qt6::Widgets
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../_migration_pack/prebuilt_cuda/bin/libring_recon_cuda.dll.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/zeromq/lib/libzmq.dll.a")
    set_target_properties(ring_timevar_ui_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ui-isolated")
    add_test(NAME ring_timevar_ui_test COMMAND ring_timevar_ui_test)
    set_tests_properties(ring_timevar_ui_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_custom_command(TARGET ring_timevar_ui_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../_migration_pack/prebuilt_cuda/bin/ring_recon_cuda.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../_migration_pack/prebuilt_cuda/bin/cudart64_12.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/zeromq/bin/libzmq-v141-mt-4_3_5.dll"
            "$<TARGET_FILE_DIR:ring_timevar_ui_test>")
endif()

# 显示层几何纯函数（TASKS/环形显示层优化_显示掩膜与毫米坐标_20260928-003123.md
# §验收 1）：映射往返/边界 + 掩膜行区间 LUT + LUT↔单像素判定一致性（纯 std，无 Qt）。
add_executable(ring_display_mapping_test
    ring_display_mapping_test.cpp)
target_include_directories(ring_display_mapping_test PRIVATE ../include)
target_compile_features(ring_display_mapping_test PRIVATE cxx_std_17)
add_test(NAME ring_display_mapping_test COMMAND ring_display_mapping_test)

# 显示渲染掩膜断言 + 默认恒等 + PNG 写入器掩膜行为（工作一/二渲染路径）。
# 内嵌基线 = fb6370b renderFrame/capturePngWriter render lambda 逐字转录；
# offscreen 平台先例同 diagnostic_dialog_test。
add_executable(ring_display_render_test
    ring_display_render_test.cpp
    ../src/ImagingDisplayWindow.cpp
    ../src/RingImageWidget.cpp
    ../src/RingColorBarWidget.cpp
    # 头文件显式列出（沿主工程 add_executable 先例）：AUTOMOC 需要
    ../include/ImagingDisplayWindow.h
    ../include/RingImageWidget.h
    ../include/RingColorBarWidget.h)
target_include_directories(ring_display_render_test PRIVATE ../include)
target_compile_features(ring_display_render_test PRIVATE cxx_std_17)
target_link_libraries(ring_display_render_test PRIVATE Qt6::Widgets)
add_test(NAME ring_display_render_test COMMAND ring_display_render_test)
set_tests_properties(ring_display_render_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(WIN32 AND TARGET Qt6::QOffscreenIntegrationPlugin)
    add_custom_command(TARGET ring_display_render_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ring_display_render_test>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
            "$<TARGET_FILE_DIR:ring_display_render_test>/platforms/")
endif()

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
    # FILESAVER_TEST_SEAM 只开给本目标（该目标自行编译一份 FileSaver.cpp），
    # 生产目标 MC410T_Receiver 的 FileSaver 不含任何故障注入口。
    target_compile_definitions(filesaver_round_boundary_test PRIVATE
        FILESAVER_TEST_SEAM WIN32_LEAN_AND_MEAN NOMINMAX)
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

# Keep the CPU DSP kernel optimized in Debug too; preserve symbols and no fast-math.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../src/RingTimevarLowpass.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/FilteredDasPrefilter.cpp PROPERTIES COMPILE_OPTIONS "-O2")
endif()

add_executable(filtered_das_test filtered_das_test.cpp ../src/FilteredDasPrefilter.cpp ../src/FilteredDasSettings.cpp)
target_include_directories(filtered_das_test PRIVATE ../include)
target_link_libraries(filtered_das_test PRIVATE Qt6::Core)
target_compile_definitions(filtered_das_test PRIVATE FILTERED_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/reference/filtered_das_fixtures")
add_test(NAME filtered_das_test COMMAND filtered_das_test)

if(WIN32)
    add_executable(filtered_das_service_test filtered_das_service_test.cpp
        ../src/ImagingSvc/ImagingSvc.cpp ../src/ImagingSvc/ImagingSvc.h ../src/ImagingSvc/ProcessScheduling.cpp
        ../src/RingRecon/ring_recon.cpp ../src/RingTimevarLowpass.cpp ../src/RingTimevarLowpassSettings.cpp ../src/FilteredDasSettings.cpp ../src/FilteredDasPrefilter.cpp)
    target_include_directories(filtered_das_service_test PRIVATE ../include ../src/RingRecon ../src/ImagingSvc
        ../libs/imaging ../third_party/zeromq/include)
    target_compile_definitions(filtered_das_service_test PRIVATE RING_TIMEVAR_TEST_SEAM WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(filtered_das_service_test PRIVATE Qt6::Core ws2_32
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../_migration_pack/prebuilt_cuda/bin/libring_recon_cuda.dll.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/zeromq/lib/libzmq.dll.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../libs/imaging/pa_recon_core.lib")
    add_custom_command(TARGET filtered_das_service_test POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../_migration_pack/prebuilt_cuda/bin/ring_recon_cuda.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../_migration_pack/prebuilt_cuda/bin/cudart64_12.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/../libs/imaging/pa_recon_core.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/../libs/imaging/cufft64_12.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/zeromq/bin/libzmq-v141-mt-4_3_5.dll"
        "$<TARGET_FILE_DIR:filtered_das_service_test>")
    add_test(NAME filtered_das_service_test COMMAND filtered_das_service_test)
    set_tests_properties(filtered_das_service_test PROPERTIES RUN_SERIAL TRUE)
    target_compile_definitions(filtered_das_service_test PRIVATE FILTERED_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/reference/filtered_das_fixtures")
endif()

if(WIN32)
    add_executable(filtered_das_ui_test filtered_das_ui_test.cpp
        ../src/RingConfigDialog.cpp ../include/RingConfigDialog.h
        ../src/ImagingController.cpp ../include/ImagingController.h
        ../src/FrontendFilter.cpp ../src/RingTimevarLowpass.cpp ../src/RingTimevarLowpassSettings.cpp ../src/FilteredDasSettings.cpp ../src/FilteredDasPrefilter.cpp)
    target_include_directories(filtered_das_ui_test PRIVATE ../include ../src/RingRecon
        ../third_party/zeromq/include)
    target_compile_definitions(filtered_das_ui_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(filtered_das_ui_test PRIVATE Qt6::Widgets
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../_migration_pack/prebuilt_cuda/bin/libring_recon_cuda.dll.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/zeromq/lib/libzmq.dll.a")
    set_target_properties(filtered_das_ui_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/filtered-ui-isolated")
    add_test(NAME filtered_das_ui_test COMMAND filtered_das_ui_test)
    set_tests_properties(filtered_das_ui_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_custom_command(TARGET filtered_das_ui_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../_migration_pack/prebuilt_cuda/bin/ring_recon_cuda.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../_migration_pack/prebuilt_cuda/bin/cudart64_12.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/zeromq/bin/libzmq-v141-mt-4_3_5.dll"
            "$<TARGET_FILE_DIR:filtered_das_ui_test>")
endif()

add_executable(filtered_das_off_test filtered_das_off_test.cpp ../src/RingTimevarLowpass.cpp ../src/RingRecon/ring_recon.cpp ../src/FilteredDasPrefilter.cpp)
target_include_directories(filtered_das_off_test PRIVATE ../include ../src/RingRecon)
target_compile_definitions(filtered_das_off_test PRIVATE FILTERED_OFF_BASELINE="${CMAKE_CURRENT_SOURCE_DIR}/reference/filtered_das_baseline_off.csv")
add_test(NAME filtered_das_off_test COMMAND filtered_das_off_test)
# 电机参数联动（LabVIEW -> 本程序）：解析/换算规则/监视器变化检测 + 组包器
# sectorCcw 顺时针角度镜像（与 ring_block_assembler_test 同口径的内轮断言）。
add_executable(motor_scan_linkage_test
    motor_scan_linkage_test.cpp
    ../src/MotorScanLinkage.cpp ../include/MotorScanLinkage.h
    ../src/RingBlockAssembler.cpp ../src/PaimageAcquisition/PhysicalRoundNormalizer.cpp)
target_include_directories(motor_scan_linkage_test PRIVATE ../include)
target_compile_features(motor_scan_linkage_test PRIVATE cxx_std_17)
target_link_libraries(motor_scan_linkage_test PRIVATE Qt6::Core)
add_test(NAME motor_scan_linkage_test COMMAND motor_scan_linkage_test)
