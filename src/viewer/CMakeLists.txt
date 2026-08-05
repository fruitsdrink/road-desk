add_executable(viewer WIN32
  main.cpp
  connect_config.cpp
  gateway_config.cpp
  demo_book.cpp
  address_book.cpp
  session_host.cpp
  console_window.cpp
  viewer.manifest
  ${CMAKE_SOURCE_DIR}/src/ui/rd_app.rc
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
  gdiplus
)

set_target_properties(viewer PROPERTIES OUTPUT_NAME "viewer")

add_custom_command(TARGET viewer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:viewer>/assets/brand"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/assets/brand/rd_login_brand.png"
    "$<TARGET_FILE_DIR:viewer>/assets/brand/rd_login_brand.png"
  COMMENT "Copy login brand image next to viewer.exe"
)

if(MSVC)
  target_link_options(viewer PRIVATE
    "/MANIFESTUAC:level='asInvoker' uiAccess='false'"
  )
endif()
