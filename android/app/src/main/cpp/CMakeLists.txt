cmake_minimum_required(VERSION 3.22)
project(xcast LANGUAGES CXX)

add_library(xcast SHARED
    jni_bridge.cpp
    sink.cpp
    rtsp_client.cpp
    rtcp_session.cpp
    stream_receiver.cpp
    video_decoder.cpp
    audio_player.cpp
    uibc_client.cpp
    cursor_overlay.cpp)

target_compile_options(xcast PRIVATE -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections
    -Werror=unguarded-availability)
# Newer-than-minSdk APIs (e.g. ASurfaceControl_fromJava, API 34) link weakly and must sit behind
# __builtin_available checks; the flag above makes a missing check a build error.
target_compile_definitions(xcast PRIVATE __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__)
target_link_options(xcast PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384)
target_link_libraries(xcast PRIVATE android log mediandk aaudio jnigraphics)
