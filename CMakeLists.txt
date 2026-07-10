cmake_minimum_required(VERSION 3.20)

project(librfb)

add_library(rfb
    rfb.hpp
    rfb.cpp
)

target_link_libraries(rfb PUBLIC
    z
    cpp_helper
    socket_helper
    avcodec
    avdevice
    avutil
)

add_executable(rfb_demo
    main.cpp
)

target_compile_features(rfb_demo PUBLIC cxx_std_20)

target_link_libraries(rfb_demo PUBLIC
    vulkan_start
    rfb
)

add_executable(rfb_window_demo
    window.cpp
)

target_compile_features(rfb_window_demo PUBLIC cxx_std_20)

target_link_libraries(rfb_window_demo PUBLIC
    vulkan_start
    posix_cpp
    xkb_helper
    rfb
)
