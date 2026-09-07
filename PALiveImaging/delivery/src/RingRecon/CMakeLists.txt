cmake_minimum_required(VERSION 3.21)
project(RingRecon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MINGW_RUNTIME_DIR "D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin"
    CACHE PATH "MinGW runtime DLL directory")

# 将 MinGW 运行库复制到目标旁边，exe 可直接双击/命令行运行，无需手工加 PATH
function(deploy_mingw_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MINGW_RUNTIME_DIR}/libgcc_s_seh-1.dll" "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MINGW_RUNTIME_DIR}/libstdc++-6.dll" "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MINGW_RUNTIME_DIR}/libwinpthread-1.dll" "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Deploying MinGW runtime for ${target}")
endfunction()

add_library(ring_recon STATIC ring_recon.cpp)
target_include_directories(ring_recon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(ring_recon_verify ring_recon_verify.cpp)
target_link_libraries(ring_recon_verify PRIVATE ring_recon)
deploy_mingw_runtime(ring_recon_verify)

option(BUILD_RINGRECON_VIEW "Build Qt GUI viewer" ON)
if(BUILD_RINGRECON_VIEW)
    find_package(Qt6 QUIET COMPONENTS Core Gui Widgets)
    if(Qt6_FOUND)
        add_executable(ring_recon_view ring_recon_view.cpp)
        target_link_libraries(ring_recon_view PRIVATE ring_recon Qt6::Widgets)
        set_target_properties(ring_recon_view PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

        # CUDA 引擎支持：提供 MSVC 构建的 ring_recon_cuda.dll 导入库即可启用
        set(RING_RECON_CUDA_IMPORT_LIB ""
            CACHE FILEPATH "MinGW import lib for ring_recon_cuda.dll (libring_recon_cuda.dll.a)")
        set(RING_RECON_CUDA_BIN ""
            CACHE PATH "Directory containing ring_recon_cuda.dll and cudart64_12.dll")
        if(RING_RECON_CUDA_IMPORT_LIB AND EXISTS "${RING_RECON_CUDA_IMPORT_LIB}")
            target_compile_definitions(ring_recon_view PRIVATE RING_RECON_VIEW_CUDA=1)
            target_link_libraries(ring_recon_view PRIVATE "${RING_RECON_CUDA_IMPORT_LIB}")
            if(RING_RECON_CUDA_BIN)
                add_custom_command(TARGET ring_recon_view POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${RING_RECON_CUDA_BIN}/ring_recon_cuda.dll" "$<TARGET_FILE_DIR:ring_recon_view>"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${RING_RECON_CUDA_BIN}/cudart64_12.dll" "$<TARGET_FILE_DIR:ring_recon_view>"
                    COMMENT "Deploying ring_recon_cuda runtime for viewer")
            endif()
        else()
            message(STATUS "ring_recon_cuda import lib not found; viewer uses CPU engine only")
        endif()

        get_target_property(_qmake_exec Qt6::qmake IMPORTED_LOCATION)
        get_filename_component(_qt_bin_dir "${_qmake_exec}" DIRECTORY)
        find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}")
        if(WINDEPLOYQT_EXECUTABLE)
            add_custom_command(TARGET ring_recon_view POST_BUILD
                COMMAND "${WINDEPLOYQT_EXECUTABLE}" --no-translations
                        "$<TARGET_FILE:ring_recon_view>"
                COMMENT "Deploying Qt runtime for ring_recon_view")
        endif()
        deploy_mingw_runtime(ring_recon_view)
    else()
        message(STATUS "Qt6 Widgets not found; skip ring_recon_view")
    endif()
endif()

if(NOT MSVC)
    target_compile_options(ring_recon PRIVATE -O2)
endif()