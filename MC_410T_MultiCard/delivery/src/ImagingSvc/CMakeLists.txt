cmake_minimum_required(VERSION 3.21)
project(ImagingSvc VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ============================================================
# Qt 依赖（仅 Core）
# ============================================================
find_package(Qt6 REQUIRED COMPONENTS Core)

# ============================================================
# 源文件
# ============================================================
add_executable(ImagingSvc
    main.cpp
    ImagingSvc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../RingRecon/ring_recon.cpp
)

# ============================================================
# 头文件路径
# ============================================================
target_include_directories(ImagingSvc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/imaging
    ${CMAKE_CURRENT_SOURCE_DIR}/../RingRecon
)

# ============================================================
# 链接库
# ============================================================
target_link_libraries(ImagingSvc PRIVATE
    Qt6::Core
)

if(WIN32)
    target_link_libraries(ImagingSvc PRIVATE ws2_32)
endif()

# ============================================================
# ZeroMQ 集成
# ============================================================
if(NOT IMAGING_ZMQ_DIR)
    set(IMAGING_ZMQ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/zeromq")
endif()
if(NOT IMAGING_ZMQ_IMPORT_LIB)
    if(MSVC)
        set(IMAGING_ZMQ_IMPORT_LIB "${IMAGING_ZMQ_DIR}/lib/libzmq-v141-mt-4_3_5.lib")
    else()
        set(IMAGING_ZMQ_IMPORT_LIB "${IMAGING_ZMQ_DIR}/lib/libzmq.dll.a")
    endif()
endif()

target_include_directories(ImagingSvc PRIVATE "${IMAGING_ZMQ_DIR}/include")
target_link_libraries(ImagingSvc PRIVATE "${IMAGING_ZMQ_IMPORT_LIB}")

# ============================================================
# 成像算法库
# ============================================================
target_link_libraries(ImagingSvc PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/imaging/pa_recon_core.lib"
)

# 环形扫描 CUDA 模块（并行分支）
if(RING_RECON_CUDA_IMPORT_LIB AND EXISTS "${RING_RECON_CUDA_IMPORT_LIB}")
    target_link_libraries(ImagingSvc PRIVATE "${RING_RECON_CUDA_IMPORT_LIB}")
endif()

# ============================================================
# 输出到主程序 bin 目录
# ============================================================
set_target_properties(ImagingSvc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ============================================================
# POST_BUILD：部署运行时 DLL
# ============================================================
add_custom_command(TARGET ImagingSvc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/imaging/pa_recon_core.dll"
        $<TARGET_FILE_DIR:ImagingSvc>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/imaging/cufft64_12.dll"
        $<TARGET_FILE_DIR:ImagingSvc>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${IMAGING_ZMQ_DIR}/bin/libzmq-v141-mt-4_3_5.dll"
        $<TARGET_FILE_DIR:ImagingSvc>
    COMMENT "Deploying ImagingSvc runtime DLLs"
)

# 环形 CUDA 运行库（与线性分支并行）
if(RING_RECON_CUDA_BIN)
    add_custom_command(TARGET ImagingSvc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RING_RECON_CUDA_BIN}/ring_recon_cuda.dll" $<TARGET_FILE_DIR:ImagingSvc>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RING_RECON_CUDA_BIN}/cudart64_12.dll" $<TARGET_FILE_DIR:ImagingSvc>
        COMMENT "Deploying ring_recon_cuda runtime")
endif()
