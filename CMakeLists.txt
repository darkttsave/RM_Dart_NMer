cmake_minimum_required(VERSION 3.10)
project(HaiKangCameraTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(Threads REQUIRED)

include_directories(${OpenCV_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/camera/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/ImgProcessing)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/Communication)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/Tool)
include_directories(/usr/local/include/RMVL)

find_path(HIK_SDK_INCLUDE_DIR MvCameraControl.h
    PATHS
    /usr/include
    /usr/local/include
    /opt/hikvision/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/hikvision/include
    ${CMAKE_CURRENT_SOURCE_DIR}/SDK/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/include
    C:/Program Files/Hikvision/include
    C:/Hikvision/include
    D:/Hikvision/include
    NO_DEFAULT_PATH
)

find_library(HIK_SDK_LIBRARY
    NAMES MvCameraControl libMvCameraControl
    PATHS
    /usr/lib
    /usr/local/lib
    /opt/hikvision/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/hikvision/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/SDK/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/lib
    C:/Program Files/Hikvision/lib
    C:/Hikvision/lib
    D:/Hikvision/lib
    NO_DEFAULT_PATH
)

if(NOT HIK_SDK_INCLUDE_DIR)
    message(FATAL_ERROR "Hikvision SDK include directory not found")
endif()

if(NOT HIK_SDK_LIBRARY)
    message(FATAL_ERROR "Hikvision SDK library not found")
endif()

message(STATUS "Found Hikvision SDK include: ${HIK_SDK_INCLUDE_DIR}")
message(STATUS "Found Hikvision SDK library: ${HIK_SDK_LIBRARY}")

set(ALL_SOURCES
    camera/src/hik_camera/hik_camera.cpp
    CameraDriver/HaiKang.cpp
    ImgProcessing/GreenDetect.cpp
    Communication/ByteConverter.cpp
    Communication/CRC16.cpp
    Communication/VirtualSerial.cpp
    Tool/Utils.cpp
    main.cpp
)

include_directories(${HIK_SDK_INCLUDE_DIR})

# 调试数据发布层（M1）：不依赖海康 SDK，便于单独复用与测试
add_library(web_debug STATIC
    web_debug/DebugTelemetry.cpp
    web_debug/FrameSource.cpp
    web_debug/Recorder.cpp
)
target_link_libraries(web_debug
    ${OpenCV_LIBS}
    Threads::Threads
)

add_executable(main ${ALL_SOURCES})

target_link_libraries(main
    ${OpenCV_LIBS}
    yaml-cpp
    ${HIK_SDK_LIBRARY}
    Threads::Threads
    web_debug
)

# 让 main 可直接运行（海康 SDK 的 .so 位于 SDK/lib，不进系统路径）
get_filename_component(HIK_SDK_LIB_DIR "${HIK_SDK_LIBRARY}" DIRECTORY)
if(HIK_SDK_LIB_DIR)
    set_target_properties(main PROPERTIES BUILD_RPATH "${HIK_SDK_LIB_DIR}")
endif()

add_executable(test_green_detect
    test/test_green_detect.cpp
    ImgProcessing/GreenDetect.cpp
)

target_link_libraries(test_green_detect
    ${OpenCV_LIBS}
    yaml-cpp
)
