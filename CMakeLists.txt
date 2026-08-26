cmake_minimum_required(VERSION 3.20)

project(librfb)

find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)

if(WIN32)
    # vcpkg ships a FindFFMPEG module that resolves the whole set of av*
    # libraries in one go, and TBB only has a config-mode package there.
    find_package(FFMPEG REQUIRED)
    find_package(TBB CONFIG REQUIRED)
    set(RFB_FFMPEG_LIBS ${FFMPEG_LIBRARIES})
    set(RFB_FFMPEG_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIRS})
    set(RFB_TBB_LIBS TBB::tbb)
    # ws2_32 provides Winsock; the rest silences MSVC's CRT/min-max noise.
    set(RFB_PLATFORM_LIBS ws2_32)
    set(RFB_PLATFORM_DEFINITIONS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
    set(RFB_FFMPEG_LIBS avcodec avdevice avutil)
    set(RFB_FFMPEG_INCLUDE_DIRS "")
    set(RFB_TBB_LIBS tbb)
    set(RFB_PLATFORM_LIBS "")
    set(RFB_PLATFORM_DEFINITIONS "")
endif()

add_library(rfb
    rfb.hpp
    rfb.cpp
)

target_compile_features(rfb PUBLIC cxx_std_20)

target_include_directories(rfb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RFB_FFMPEG_INCLUDE_DIRS}
)

target_compile_definitions(rfb PUBLIC ${RFB_PLATFORM_DEFINITIONS})

target_link_libraries(rfb PUBLIC
    ZLIB::ZLIB
    cpp_helper
    socket_helper
    ${RFB_FFMPEG_LIBS}
    OpenSSL::Crypto
    ${RFB_TBB_LIBS}
    ${RFB_PLATFORM_LIBS}
)

if(MSVC)
    # rfb.hpp and the tools are UTF-8 encoded; without /utf-8 MSVC reads them
    # in the active code page. /Zc:__cplusplus makes __cplusplus report C++20.
    target_compile_options(rfb PUBLIC /utf-8 /Zc:__cplusplus /EHsc)
endif()

add_executable(rfb_demo
    main.cpp
)

target_compile_features(rfb_demo PUBLIC cxx_std_20)

target_link_libraries(rfb_demo PUBLIC
    rfb
)

add_executable(rfb_recorder
    recorder.cpp
)

target_compile_features(rfb_recorder PUBLIC cxx_std_20)

if(WIN32)
    set(RFB_RECORDER_EXTRA_LIBS "")
    set(RFB_SWSCALE_LIBS "")
else()
    set(RFB_RECORDER_EXTRA_LIBS avformat)
    set(RFB_SWSCALE_LIBS swscale)
endif()

target_link_libraries(rfb_recorder PUBLIC
    rfb
    ${RFB_RECORDER_EXTRA_LIBS}
    ${RFB_SWSCALE_LIBS}
)

add_executable(rfb_input
    input.cpp
)

target_compile_features(rfb_input PUBLIC cxx_std_20)

target_link_libraries(rfb_input PUBLIC
    rfb
    ${RFB_SWSCALE_LIBS}
)

# The window demo and the HIP test render through Wayland + DRM/amdgpu + HIP
# external memory, which have no Windows equivalent.
if(UNIX)
    add_executable(rfb_window_demo
        window.cpp
    )

    target_compile_features(rfb_window_demo PUBLIC cxx_std_20)

    target_link_libraries(rfb_window_demo PUBLIC
        posix_cpp
        xkb_helper
        wayland_helper
        hip_helper
        drm_helper
        rfb
    )

    add_executable(test_hip
        test_hip.cpp
    )

    target_compile_features(test_hip PUBLIC cxx_std_20)

    target_link_libraries(test_hip PUBLIC
        posix_cpp
        xkb_helper
        wayland_helper
        hip_helper
        drm_helper
        rfb
    )
endif()
