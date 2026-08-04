# Mux media plane (private TLS protocol + Mirror/GDI capture). No LibVNC.
option(ROAD_DESK_WITH_MEDIA "Build full mux media plane (OFF = stub)" ON)

set(MIRROR_USER_DIR "${CMAKE_SOURCE_DIR}/spikes/mirror-driver/user")
set(MINIZ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz")

if(ROAD_DESK_WITH_MEDIA)
  add_library(road_desk_miniz STATIC
    third_party/miniz/miniz.c
    third_party/miniz/miniz_tdef.c
    third_party/miniz/miniz_tinfl.c
  )
  target_include_directories(road_desk_miniz PUBLIC "${MINIZ_DIR}")
  target_compile_definitions(road_desk_miniz PUBLIC
    MINIZ_NO_STDIO
    MINIZ_NO_TIME
    MINIZ_NO_ARCHIVE_APIS
    MINIZ_NO_ARCHIVE_WRITING_APIS
  )
  if(MSVC)
    target_compile_options(road_desk_miniz PRIVATE /W3 /wd4996)
  endif()

  add_library(road_desk_media_host STATIC
    mux_host.cpp
    mux.cpp
    mux_clipboard.cpp
    mux_file_xfer.cpp
    capture.cpp
    capture_resolve.cpp
    mirror_capture.cpp
    dxgi_capture.cpp
    os_version.cpp
    video_encode.cpp
    cursor_capture.cpp
    mux_inject.cpp
    media_log.cpp
    tls_schannel.cpp
    tls_mvp_cert_data.cpp
    "${MIRROR_USER_DIR}/mirror_client.cpp"
  )
  target_include_directories(road_desk_media_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_include_directories(road_desk_media_host PRIVATE
    "${MIRROR_USER_DIR}"
    "${MINIZ_DIR}"
  )
  target_compile_definitions(road_desk_media_host PUBLIC ROAD_DESK_WITH_MEDIA=1)
  target_link_libraries(road_desk_media_host PUBLIC
    road_desk_session
    road_desk_miniz
    ws2_32 gdi32 user32 shell32 secur32 crypt32 ncrypt dwmapi
  )

  add_library(road_desk_media_client STATIC
    mux_client.cpp
    mux.cpp
    mux_clipboard.cpp
    mux_file_xfer.cpp
    media_log.cpp
    tls_schannel.cpp
    tls_mvp_cert_data.cpp
  )
  target_include_directories(road_desk_media_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_include_directories(road_desk_media_client PRIVATE "${MINIZ_DIR}")
  target_compile_definitions(road_desk_media_client PUBLIC ROAD_DESK_WITH_MEDIA=1)
  target_link_libraries(road_desk_media_client PUBLIC
    road_desk_session
    road_desk_miniz
    ws2_32 gdi32 user32 shell32 secur32 crypt32 ncrypt
  )

  add_library(road_desk_media INTERFACE)
  target_link_libraries(road_desk_media INTERFACE road_desk_media_host)

  # Lightweight host-side unit tests (no gtest): strategy matrix + video encode.
  add_executable(capture_strategy_test
    tests/capture_strategy_test.cpp
    capture_resolve.cpp
    os_version.cpp
    video_encode.cpp
  )
  target_include_directories(capture_strategy_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${MINIZ_DIR}"
  )
  target_compile_definitions(capture_strategy_test PRIVATE ROAD_DESK_WITH_MEDIA=1)
  target_link_libraries(capture_strategy_test PRIVATE road_desk_miniz)
  add_test(NAME capture_strategy_test COMMAND capture_strategy_test)
else()
  add_library(road_desk_media STATIC
    media_stub.cpp
  )
  target_include_directories(road_desk_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  add_library(road_desk_media_host ALIAS road_desk_media)
  add_library(road_desk_media_client ALIAS road_desk_media)
endif()
