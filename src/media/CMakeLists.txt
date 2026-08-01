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
    capture.cpp
    mirror_capture.cpp
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
    ws2_32 gdi32 user32 secur32 crypt32 ncrypt dwmapi
  )

  add_library(road_desk_media_client STATIC
    mux_client.cpp
    mux.cpp
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
    ws2_32 gdi32 user32 secur32 crypt32 ncrypt
  )

  add_library(road_desk_media INTERFACE)
  target_link_libraries(road_desk_media INTERFACE road_desk_media_host)
else()
  add_library(road_desk_media STATIC
    media_stub.cpp
  )
  target_include_directories(road_desk_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  add_library(road_desk_media_host ALIAS road_desk_media)
  add_library(road_desk_media_client ALIAS road_desk_media)
endif()
