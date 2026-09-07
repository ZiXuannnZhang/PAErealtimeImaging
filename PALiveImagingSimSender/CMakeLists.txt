cmake_minimum_required(VERSION 3.21)
project(PALiveImagingSimSender VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

set(CMAKE_PREFIX_PATH "D:/Qt/Qt6.8.0/6.8.0/mingw_64")

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(PALiveImagingSimSender
    src/main.cpp
    src/SimSenderWindow.cpp
    src/UdpReplaySender.cpp
    src/VirtualCardEmulator.cpp
)

target_include_directories(PALiveImagingSimSender PRIVATE src)
target_link_libraries(PALiveImagingSimSender PRIVATE Qt6::Widgets ws2_32)

set_target_properties(PALiveImagingSimSender PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# 部署 Qt 运行库到输出目录
add_custom_command(TARGET PALiveImagingSimSender POST_BUILD
    COMMAND D:/Qt/Qt6.8.0/6.8.0/mingw_64/bin/windeployqt.exe
            --no-translations "$<TARGET_FILE:PALiveImagingSimSender>"
    COMMENT "Deploying Qt runtime")
