cmake_minimum_required(VERSION 3.20)

project(librfb)

include(CTest)

find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)

if(NOT TARGET remote_media::remote_media)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../remote-media/CMakeLists.txt")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../remote-media"
                         "${CMAKE_CURRENT_BINARY_DIR}/remote-media")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/remote-media/CMakeLists.txt")
        add_subdirectory(third_party/remote-media)
    endif()
endif()

if(NOT TARGET remote_media::remote_media)
    message(FATAL_ERROR
        "librfb server support requires remote_media::remote_media; add the "
        "remote-media submodule or provide the target before add_subdirectory(librfb)")
endif()

add_library(rfb_server_core
    server_core.cpp
    server_core.hpp
    server.hpp
    server_encode.hpp
    server_h264.hpp
    server_platform.hpp
    keysym.hpp
)
target_compile_features(rfb_server_core PUBLIC cxx_std_20)
target_include_directories(rfb_server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rfb_server_core PUBLIC
    remote_media::remote_media
    cpp_helper
    socket_helper
    ZLIB::ZLIB
    OpenSSL::Crypto
)
add_library(rfb::server_core ALIAS rfb_server_core)

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
    # swscale: h264.hpp converts the decoded YUV picture to BGRA with it.
    set(RFB_FFMPEG_LIBS avcodec avdevice avutil swscale)
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

# rfb_input sends input events and never touches pixels, so it does not link
# swscale directly. (It still gets it transitively through rfb, which needs it
# for H264 decoding.)
target_link_libraries(rfb_input PUBLIC
    rfb
)

add_executable(rfb_capture
    capture.cpp
)

target_compile_features(rfb_capture PUBLIC cxx_std_20)

target_link_libraries(rfb_capture PUBLIC
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
    rfb::server_core
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
    # The release-supported Windows viewer path is read-only display. The
    # executable contains experimental input, clipboard and control plumbing,
    # but those broader interaction paths have not been release-validated.
    add_executable(rfb_window_win32
        windows_viewer.cpp
        viewer_client.hpp
        control_server.hpp
        trace.hpp
    )
    target_compile_features(rfb_window_win32 PRIVATE cxx_std_20)
    target_link_libraries(rfb_window_win32 PRIVATE
        rfb
        remote_media::remote_media
        d3d11 dxgi user32 gdi32
    )
    target_compile_definitions(rfb_window_win32 PRIVATE
        ${RFB_PLATFORM_DEFINITIONS})
    if(MSVC)
        target_compile_options(rfb_window_win32 PRIVATE /utf-8 /Zc:__cplusplus /EHsc)
    endif()

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

if(BUILD_TESTING)
    add_executable(rfb_control_tests test_control.cpp)
    target_compile_features(rfb_control_tests PRIVATE cxx_std_20)
    target_link_libraries(rfb_control_tests PRIVATE rfb)
    add_test(NAME rfb_control_tests COMMAND rfb_control_tests)

    add_executable(rfb_trace_tests test_trace.cpp)
    target_compile_features(rfb_trace_tests PRIVATE cxx_std_20)
    target_link_libraries(rfb_trace_tests PRIVATE rfb)
    add_test(NAME rfb_trace_tests COMMAND rfb_trace_tests)

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME rfb_ctl_python_tests
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_rfb_ctl.py)
    endif()

    add_executable(rfb_server_core_tests test_server_core.cpp)
    target_compile_features(rfb_server_core_tests PRIVATE cxx_std_20)
    target_link_libraries(rfb_server_core_tests PRIVATE
        rfb::server_core
        ${RFB_PLATFORM_LIBS}
    )
    if(WIN32)
        target_link_libraries(rfb_server_core_tests PRIVATE gdi32 user32)
        target_compile_definitions(rfb_server_core_tests PRIVATE
            ${RFB_PLATFORM_DEFINITIONS})
    else()
        target_link_libraries(rfb_server_core_tests PRIVATE Threads::Threads)
    endif()
    add_test(NAME rfb_server_core_tests COMMAND rfb_server_core_tests)
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
