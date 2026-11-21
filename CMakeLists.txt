cmake_minimum_required(VERSION 3.10)
project(HaiKangCameraTest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(OpenCV REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(Threads REQUIRED)

find_path(HIK_SDK_INCLUDE_DIR MvCameraControl.h
    PATHS /usr/include /usr/local/include /opt/hikvision/include
          ${PROJECT_SOURCE_DIR}/third_party/hikvision/include
          ${PROJECT_SOURCE_DIR}/SDK/include ${PROJECT_SOURCE_DIR}/lib/include
          "C:/Program Files/Hikvision/include" C:/Hikvision/include D:/Hikvision/include)
find_library(HIK_SDK_LIBRARY NAMES MvCameraControl libMvCameraControl
    PATHS /usr/lib /usr/local/lib /opt/hikvision/lib
          ${PROJECT_SOURCE_DIR}/third_party/hikvision/lib
          ${PROJECT_SOURCE_DIR}/SDK/lib ${PROJECT_SOURCE_DIR}/lib/lib
          "C:/Program Files/Hikvision/lib" C:/Hikvision/lib D:/Hikvision/lib)
if(NOT HIK_SDK_INCLUDE_DIR OR NOT HIK_SDK_LIBRARY)
    message(FATAL_ERROR "Hikvision SDK include directory or library not found")
endif()

add_library(dart_io STATIC
    camera/src/hik_camera/hik_camera.cpp io/camera/HaiKang.cpp
    io/serial/ByteConverter.cpp io/serial/CRC16.cpp io/serial/VirtualSerial.cpp)
target_include_directories(dart_io PUBLIC ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/camera/include ${HIK_SDK_INCLUDE_DIR} ${OpenCV_INCLUDE_DIRS})
target_link_libraries(dart_io PUBLIC ${OpenCV_LIBS} yaml-cpp ${HIK_SDK_LIBRARY})

add_library(dart_vision STATIC tasks/dart/GreenDetect.cpp)
target_include_directories(dart_vision PUBLIC ${PROJECT_SOURCE_DIR} ${OpenCV_INCLUDE_DIRS})
target_link_libraries(dart_vision PUBLIC ${OpenCV_LIBS} yaml-cpp)

add_library(dart_debug STATIC debug/DebugTelemetry.cpp debug/FrameSource.cpp debug/Recorder.cpp debug/RuntimeParams.cpp)
target_include_directories(dart_debug PUBLIC ${PROJECT_SOURCE_DIR} ${OpenCV_INCLUDE_DIRS})
target_link_libraries(dart_debug PUBLIC ${OpenCV_LIBS} Threads::Threads)

add_library(dart_tools STATIC tools/Utils.cpp)
target_include_directories(dart_tools PUBLIC ${PROJECT_SOURCE_DIR} ${OpenCV_INCLUDE_DIRS})
target_link_libraries(dart_tools PUBLIC ${OpenCV_LIBS})

add_executable(main src/main.cpp)
target_link_libraries(main PRIVATE dart_io dart_vision dart_debug dart_tools yaml-cpp Threads::Threads)
get_filename_component(HIK_SDK_LIB_DIR "${HIK_SDK_LIBRARY}" DIRECTORY)
set_target_properties(main PROPERTIES BUILD_RPATH "${HIK_SDK_LIB_DIR}")

add_executable(test_green_detect tests/test_green_detect.cpp)
target_link_libraries(test_green_detect PRIVATE dart_vision)

add_executable(camera_test tests/camera_test.cpp)
target_link_libraries(camera_test PRIVATE dart_io)
set_target_properties(camera_test PROPERTIES BUILD_RPATH "${HIK_SDK_LIB_DIR}")
