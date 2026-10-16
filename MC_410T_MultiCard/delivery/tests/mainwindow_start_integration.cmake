# Reuse the production target's complete sources and links, replacing only main().
# MainWindow, controller, assembler, feed and ImagingSvc are all real implementations.
get_target_property(_startup_sources MC410T_Receiver SOURCES)
list(REMOVE_ITEM _startup_sources src/main.cpp)
add_executable(mainwindow_start_integration_test
    ${_startup_sources} tests/mainwindow_start_integration_test.cpp)
foreach(_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_LIBRARIES)
    get_target_property(_startup_value MC410T_Receiver ${_property})
    set_property(TARGET mainwindow_start_integration_test PROPERTY ${_property} "${_startup_value}")
endforeach()
target_compile_definitions(mainwindow_start_integration_test PRIVATE MAINWINDOW_START_TEST_SEAM)
set_target_properties(mainwindow_start_integration_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/startup-isolated")
add_dependencies(mainwindow_start_integration_test ImagingSvc)
add_custom_command(TARGET mainwindow_start_integration_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:ImagingSvc>"
        "${IMAGING_ZMQ_DIR}/bin/libzmq-v141-mt-4_3_5.dll"
        "${CMAKE_SOURCE_DIR}/libs/imaging/pa_recon_core.dll"
        "${CMAKE_SOURCE_DIR}/libs/imaging/cufft64_12.dll"
        "${RING_RECON_CUDA_BIN}/ring_recon_cuda.dll"
        "${RING_RECON_CUDA_BIN}/cudart64_12.dll"
        "$<TARGET_FILE_DIR:mainwindow_start_integration_test>")
enable_testing()
add_test(NAME mainwindow_start_integration_test COMMAND mainwindow_start_integration_test)
set_tests_properties(mainwindow_start_integration_test PROPERTIES
    RUN_SERIAL TRUE TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
