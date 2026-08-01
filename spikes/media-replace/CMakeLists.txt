cmake_minimum_required(VERSION 3.20)
project(road-desk-media-replace LANGUAGES C CXX)

# Track P spike: private mux over Schannel TLS. No LibVNC.
# Does not modify src/media/libvnc_*.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "media-replace spike targets Windows x64 only")
endif()

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

if(MSVC)
  add_compile_options(/W3 /utf-8 /wd4996 /d2FH4-)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>")
endif()

add_library(replace_tls_session STATIC
  "${REPO_ROOT}/src/media/tls_schannel.cpp"
  "${REPO_ROOT}/src/media/tls_mvp_cert_data.cpp"
  "${REPO_ROOT}/src/session/auth.cpp"
  "${REPO_ROOT}/src/session/session_mutex.cpp"
)
target_include_directories(replace_tls_session PUBLIC
  "${REPO_ROOT}/src/media"
  "${REPO_ROOT}/src/session"
)
target_link_libraries(replace_tls_session PUBLIC ws2_32 secur32 crypt32 ncrypt)

# miniz (zlib subset) for dirty-rect compression. No archive/stdio.
add_library(replace_miniz STATIC
  third_party/miniz.c
  third_party/miniz_tdef.c
  third_party/miniz_tinfl.c
)
target_include_directories(replace_miniz PUBLIC third_party)
target_compile_definitions(replace_miniz PUBLIC
  MINIZ_NO_STDIO
  MINIZ_NO_TIME
  MINIZ_NO_ARCHIVE_APIS
  MINIZ_NO_ARCHIVE_WRITING_APIS
)

add_executable(replace_host
  src/replace_host.cpp
  src/mux.cpp
  src/capture.cpp
  src/cursor_capture.cpp
  src/inject.cpp
  src/log_util.cpp
)
target_include_directories(replace_host PRIVATE src)
target_link_libraries(replace_host PRIVATE replace_tls_session replace_miniz user32 gdi32)

add_executable(replace_viewer
  src/replace_viewer.cpp
  src/mux.cpp
  src/log_util.cpp
)
target_include_directories(replace_viewer PRIVATE src)
target_link_libraries(replace_viewer PRIVATE replace_tls_session replace_miniz user32 gdi32 msimg32)
