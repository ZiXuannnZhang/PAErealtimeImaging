cmake_minimum_required(VERSION 3.21)
project(RingReconCuda LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_ARCHITECTURES 89)

# CUDA 重建核心（DLL，C 接口，供 MinGW 上位机/ImagingSvc 后续链接）
add_library(ring_recon_cuda SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/../RingRecon/ring_recon_cuda.cu)
target_include_directories(ring_recon_cuda PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../RingRecon)
target_compile_definitions(ring_recon_cuda PRIVATE RING_RECON_CUDA_EXPORTS)

# MSVC 需要 /utf-8 才能正确解析 .cu 中的 UTF-8 中文注释（否则按 GBK 解析会破坏结构体定义）
if(MSVC)
    target_compile_options(ring_recon_cuda PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/utf-8>)
endif()

# 验证入口（MSVC + nvcc 构建，复用 CPU 版预处理）
add_executable(ring_recon_cuda_verify
    ring_recon_cuda_verify.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../RingRecon/ring_recon.cpp)
target_include_directories(ring_recon_cuda_verify PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../RingRecon)
target_link_libraries(ring_recon_cuda_verify PRIVATE ring_recon_cuda)

# MSVC 需要 /utf-8 才能正确解析 ring_recon.h/.cpp 中的中文注释
if(MSVC)
    target_compile_options(ring_recon_cuda_verify PRIVATE /utf-8)
endif()

set_target_properties(ring_recon_cuda PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 运行部署：验证程序旁边自动带上 ring_recon_cuda.dll 与 CUDA 运行时，
# 避免每次手动加 PATH。
set(CUDA_RUNTIME_DIR "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.8/bin"
    CACHE PATH "CUDA runtime DLL directory")
add_custom_command(TARGET ring_recon_cuda_verify POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:ring_recon_cuda>" "$<TARGET_FILE_DIR:ring_recon_cuda_verify>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CUDA_RUNTIME_DIR}/cudart64_12.dll" "$<TARGET_FILE_DIR:ring_recon_cuda_verify>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CUDA_RUNTIME_DIR}/cudart64_12.dll" "$<TARGET_FILE_DIR:ring_recon_cuda>"
    COMMENT "Deploying CUDA runtime DLLs"
)