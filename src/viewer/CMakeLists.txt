add_executable(viewer WIN32
  main.cpp
  connect_config.cpp
  gateway_config.cpp
  demo_book.cpp
  address_book.cpp
  session_host.cpp
  console_window.cpp
  viewer.manifest
)

target_include_directories(viewer PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(viewer PRIVATE UNICODE _UNICODE)
target_link_libraries(viewer PRIVATE
  road_desk_media_client
  road_desk_session
  comctl32
  winhttp
  comdlg32
  shell32
)

set_target_properties(viewer PROPERTIES OUTPUT_NAME "viewer")

if(MSVC)
  target_link_options(viewer PRIVATE
    "/MANIFESTUAC:level='asInvoker' uiAccess='false'"
  )
endif()
