cmake_minimum_required(VERSION 3.20)
project(road-desk-media-spike LANGUAGES C CXX)

# Throwaway media-plane spike (GPL LibVNC). Not product delivery code.
# Build after third_party/libvncserver is fetched and built into ../../build/libvncserver.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "spike targets Windows x64 only")
endif()

set(LIBVNC_SRC_ROOT "${CMAKE_SOURCE_DIR}/../../third_party/libvncserver")
set(LIBVNC_BUILD_ROOT "${CMAKE_SOURCE_DIR}/../../build/libvncserver")

if(NOT EXISTS "${LIBVNC_BUILD_ROOT}/vncserver.lib")
  message(FATAL_ERROR "LibVNC not built. Run spikes/media-plane/scripts/build.ps1")
endif()

if(MSVC)
  add_compile_options(/W3 /utf-8 /wd4996)
  # Match LibVNC default /MD — do not static-link CRT here.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(spike_host
  src/spike_host.cpp
  src/win_input.cpp
)
target_include_directories(spike_host PRIVATE
  "${LIBVNC_SRC_ROOT}/include"
  "${LIBVNC_BUILD_ROOT}/include"
)
target_link_libraries(spike_host PRIVATE
  "${LIBVNC_BUILD_ROOT}/vncserver.lib"
  ws2_32 gdi32 user32
)

add_executable(spike_viewer WIN32
  src/spike_viewer.cpp
)
target_include_directories(spike_viewer PRIVATE
  "${LIBVNC_SRC_ROOT}/include"
  "${LIBVNC_BUILD_ROOT}/include"
)
target_link_libraries(spike_viewer PRIVATE
  "${LIBVNC_BUILD_ROOT}/vncclient.lib"
  ws2_32 gdi32 user32
)
