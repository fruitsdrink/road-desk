set(LIBVNC_SRC_ROOT "${CMAKE_SOURCE_DIR}/third_party/libvncserver")
set(LIBVNC_BUILD_ROOT "${CMAKE_SOURCE_DIR}/build/libvncserver")

if(ROAD_DESK_WITH_LIBVNC)
  if(NOT EXISTS "${LIBVNC_BUILD_ROOT}/vncserver.lib" OR NOT EXISTS "${LIBVNC_BUILD_ROOT}/vncclient.lib")
    message(FATAL_ERROR
      "LibVNC libs missing under build/libvncserver. Run scripts/build.ps1 (fetches + builds LibVNC), "
      "or configure with -DROAD_DESK_WITH_LIBVNC=OFF for stubs.")
  endif()

  # Host agent must NOT link vncclient — that mismatched spike_host and crashed on Win7 before main/log.
  add_library(road_desk_media_host STATIC
    libvnc_host.cpp
    win_input.cpp
    tls_schannel.cpp
    tls_mvp_cert_data.cpp
  )
  target_include_directories(road_desk_media_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_include_directories(road_desk_media_host PRIVATE
    "${LIBVNC_SRC_ROOT}/include"
    "${LIBVNC_BUILD_ROOT}/include"
  )
  target_compile_definitions(road_desk_media_host PUBLIC ROAD_DESK_WITH_LIBVNC=1)
  target_link_libraries(road_desk_media_host PUBLIC
    road_desk_session
    "${LIBVNC_BUILD_ROOT}/vncserver.lib"
    ws2_32 gdi32 user32 secur32 crypt32 ncrypt
  )

  add_library(road_desk_media_client STATIC
    libvnc_client.cpp
    tls_schannel.cpp
    tls_mvp_cert_data.cpp
  )
  target_include_directories(road_desk_media_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_include_directories(road_desk_media_client PRIVATE
    "${LIBVNC_SRC_ROOT}/include"
    "${LIBVNC_BUILD_ROOT}/include"
  )
  target_compile_definitions(road_desk_media_client PUBLIC ROAD_DESK_WITH_LIBVNC=1)
  target_link_libraries(road_desk_media_client PUBLIC
    "${LIBVNC_BUILD_ROOT}/vncclient.lib"
    ws2_32 gdi32 user32 secur32 crypt32 ncrypt
  )

  # Compatibility alias used by docs; prefer host/client targets in executables.
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
