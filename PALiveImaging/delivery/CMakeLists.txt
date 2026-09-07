cmake_minimum_required(VERSION 3.21)
project(MC410T_Receiver VERSION 1.0.0 LANGUAGES CXX)

# ============================================================
# 全局编译设置
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/ui)

# 扩展发布：ZeroMQ FramePublisher 开关
option(USE_ZEROMQ "Enable ZeroMQ FramePublisher extension" OFF)

# ============================================================
# Qt 6 组件
# ============================================================
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets PrintSupport OpenGL OpenGLWidgets Svg)

# ============================================================
# 源文件列表
# ============================================================
set(SOURCES
    src/main.cpp
    src/DataProcessor.cpp
    src/FileSaver.cpp
    src/FramePublisher.cpp
    src/MultiPortReceiver.cpp
    src/NetworkController.cpp
    src/MainWindow.cpp
    src/ImagingController.cpp
    src/RingConfigDialog.cpp
    src/ImagingDisplayWindow.cpp
    src/RingBlockAssembler.cpp
    qcustomplot/qcustomplot.cpp
)

set(HEADERS
    include/Constants.h
    include/AcqConfig.h
    include/DataTypes.h
    include/ObjectPool.h
    include/PacketAssemblyBuffer.h
    include/DisplayBuffer.h
    include/DataProcessor.h
    include/MultiPortReceiver.h
    include/ReceiverBase.h
    include/FileSaver.h
    include/FramePublisher.h
    include/NetworkController.h
    include/MainWindow.h
    include/ImagingParams.h
    include/ImagingSharedMemory.h
    include/ImagingController.h
    include/RingConfigDialog.h
    include/ImagingDisplayWindow.h
    include/RingBlockAssembler.h
    include/third_party/concurrentqueue.h
    qcustomplot/qcustomplot.h
)

# ============================================================
# 可执行目标
# ============================================================
add_executable(${PROJECT_NAME}
    resources.qrc
    ${SOURCES}
    ${HEADERS}
)

# ============================================================
# 头文件搜索路径
# ============================================================
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/qcustomplot
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/imaging
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RingRecon
)

# ============================================================
# Qt 链接库
# ============================================================
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::PrintSupport
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Svg
    opengl32
)

# ============================================================
# Windows 平台设置
# ============================================================
if(WIN32)
    # Winsock2（MultiPortReceiver 依赖）
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 iphlpapi)

    # 抑制 Windows CRT 安全警告
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        QCUSTOMPLOT_USE_OPENGL
    )

    # MSVC：隐藏控制台窗口，生成 Windows GUI 子系统
    if(MSVC)
        set_target_properties(${PROJECT_NAME} PROPERTIES
            WIN32_EXECUTABLE TRUE
            LINK_FLAGS "/SUBSYSTEM:WINDOWS"
        )
        # 开启 AVX2 优化：FileSaver 的 float32→float16 转换使用 F16C 指令集
        target_compile_options(${PROJECT_NAME} PRIVATE /arch:AVX2)
    endif()
endif()

# ============================================================
# ZeroMQ 集成（USE_ZEROMQ=ON 时启用）
# ============================================================
if(USE_ZEROMQ)
    set(ZMQ_INSTALL "${CMAKE_SOURCE_DIR}/third_party/zeromq" CACHE PATH "ZeroMQ install directory")
    if(MSVC)
        set(ZMQ_IMPORT_LIB "libzmq-v143-mt-4_3_5.lib")
        set(ZMQ_DLL_NAME "libzmq-v143-mt-4_3_5.dll")
    else()
        set(ZMQ_IMPORT_LIB "libzmq.dll.a")
        set(ZMQ_DLL_NAME "libzmq-v141-mt-4_3_5.dll")
    endif()
    target_compile_definitions(${PROJECT_NAME} PRIVATE USE_ZEROMQ)
    target_include_directories(${PROJECT_NAME} PRIVATE "${ZMQ_INSTALL}/include")
    target_link_libraries(${PROJECT_NAME} PRIVATE "${ZMQ_INSTALL}/lib/${ZMQ_IMPORT_LIB}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ZMQ_INSTALL}/bin/${ZMQ_DLL_NAME}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMENT "Copying libzmq DLL..."
    )
endif()

# ============================================================
# 成像扩展：ZeroMQ + pa_recon_core
# ImagingController/ImagingSvc 使用 ZMQ PAIR IPC
# ============================================================
set(IMAGING_ZMQ_DIR "${CMAKE_SOURCE_DIR}/third_party/zeromq"
    CACHE PATH "ZeroMQ directory for imaging IPC")
if(MSVC)
    set(IMAGING_ZMQ_IMPORT_LIB "${IMAGING_ZMQ_DIR}/lib/libzmq-v141-mt-4_3_5.lib")
else()
    set(IMAGING_ZMQ_IMPORT_LIB "${IMAGING_ZMQ_DIR}/lib/libzmq.dll.a")
endif()
target_include_directories(${PROJECT_NAME} PRIVATE "${IMAGING_ZMQ_DIR}/include")
target_link_libraries(${PROJECT_NAME} PRIVATE "${IMAGING_ZMQ_IMPORT_LIB}")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${IMAGING_ZMQ_DIR}/bin/libzmq-v141-mt-4_3_5.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/libs/imaging/pa_recon_core.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/libs/imaging/cufft64_12.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMENT "Deploying imaging runtime DLLs"
)

# 环形扫描 CUDA 模块（与线性 pa_recon 分支并行；由 build_mingw_debug.cmd 传入路径）
set(RING_RECON_CUDA_IMPORT_LIB "" CACHE FILEPATH "MinGW import lib for ring_recon_cuda.dll")
set(RING_RECON_CUDA_BIN "" CACHE PATH "Directory containing ring_recon_cuda.dll and cudart64_12.dll")

# 成像子进程
add_subdirectory(src/ImagingSvc)

# 环形链路自检（模拟主进程启动 ImagingSvc 并喂入分块数据）
# ============================================================
# 输出目录
# ============================================================
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "MC410T_Receiver"
)

# ============================================================
# windeployqt：自动复制 Qt 运行时 DLL
# ============================================================
if(WIN32 AND Qt6_FOUND)
    get_target_property(_qmake_exec Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake_exec}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt
                 HINTS "${_qt_bin_dir}")
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                    --no-translations
                    "$<TARGET_FILE:${PROJECT_NAME}>"
            COMMENT "Running windeployqt..."
        )
    endif()
endif()
