cmake_minimum_required(VERSION 3.20)

project(librfb)

find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)

if(WIN32)
    # vcpkg ships a FindFFMPEG module that resolves the whole set of av*
    # libraries in one go.
    find_package(FFMPEG REQUIRED)
    set(RFB_FFMPEG_LIBS ${FFMPEG_LIBRARIES})
    set(RFB_FFMPEG_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIRS})
    # No TBB here. h264.hpp uses std::for_each(std::execution::par, ...), and
    # only libstdc++ farms its parallel algorithms out to TBB; the MSVC STL
    # implements them natively.
    set(RFB_TBB_LIBS "")
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

add_executable(rfb_clipboard
    clipboard.cpp
)

target_compile_features(rfb_clipboard PUBLIC cxx_std_20)

target_link_libraries(rfb_clipboard PUBLIC
    rfb
    ${RFB_SWSCALE_LIBS}
)

# The server shares only the socket helpers with the client library, so it
# deliberately does not link rfb: that would drag ffmpeg in for no reason.
add_executable(rfb_server
    server.cpp
    server.hpp
    server_encode.hpp
    server_h264.hpp
    server_platform.hpp
    keysym.hpp
)

target_compile_features(rfb_server PUBLIC cxx_std_20)

target_include_directories(rfb_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(rfb_server PUBLIC
    cpp_helper
    socket_helper
    ZLIB::ZLIB
    OpenSSL::Crypto
)

# H264 is the one thing that would drag ffmpeg into the server, so it is opt
# in: the default Windows build has to stay ffmpeg-free.
option(RFB_SERVER_H264 "Build rfb_server with H264 (encoding 50) support" OFF)

if(RFB_SERVER_H264)
    if(WIN32)
        set(RFB_SERVER_H264_LIBS ${FFMPEG_LIBRARIES})
        target_include_directories(rfb_server PRIVATE ${FFMPEG_INCLUDE_DIRS})
    else()
        set(RFB_SERVER_H264_LIBS avcodec avutil swscale)
    endif()
    target_link_libraries(rfb_server PUBLIC ${RFB_SERVER_H264_LIBS})
    target_compile_definitions(rfb_server PUBLIC RFB_SERVER_HAS_H264)
endif()

if(WIN32)
    # GDI capture and SendInput.
    target_link_libraries(rfb_server PUBLIC gdi32 user32)
    target_compile_definitions(rfb_server PUBLIC ${RFB_PLATFORM_DEFINITIONS})
else()
    find_package(Threads REQUIRED)
    target_link_libraries(rfb_server PUBLIC Threads::Threads)
endif()

if(MSVC)
    target_compile_options(rfb_server PUBLIC /utf-8 /Zc:__cplusplus /EHsc)
endif()

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
