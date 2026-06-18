cmake_minimum_required(VERSION 3.20)

project(librfb)

add_executable(rfb_demo
    main.cpp
)

target_compile_features(rfb_demo PUBLIC cxx_std_20)

target_link_libraries(rfb_demo PUBLIC vulkan_start)

