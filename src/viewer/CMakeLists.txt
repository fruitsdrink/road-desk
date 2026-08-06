set(VIEWER_SOURCES
  main.cpp
  connect_config.cpp
  gateway_config.cpp
  demo_book.cpp
  address_book.cpp
  session_host.cpp
  console_window.cpp
  audit_client.cpp
)

# When any viewer source changes, bump ROAD_DESK_VERSION_PATCH before compiling.
set(VIEWER_VERSION_STAMP "${CMAKE_BINARY_DIR}/generated/viewer_version.stamp")
find_program(ROAD_DESK_PWSH NAMES pwsh powershell REQUIRED)
add_custom_command(
  OUTPUT "${VIEWER_VERSION_STAMP}"
  COMMAND "${ROAD_DESK_PWSH}" -NoProfile -ExecutionPolicy Bypass
          -File "${CMAKE_SOURCE_DIR}/scripts/bump_product_version.ps1"
          -Stamp "${VIEWER_VERSION_STAMP}"
  DEPENDS
    ${VIEWER_SOURCES}
    viewer.manifest
    "${CMAKE_SOURCE_DIR}/src/ui/rd_app.rc"
    "${CMAKE_SOURCE_DIR}/scripts/bump_product_version.ps1"
  COMMENT "Bump product version (viewer sources changed)"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  VERBATIM
)
add_custom_target(viewer_bump_version DEPENDS "${VIEWER_VERSION_STAMP}")

add_executable(viewer WIN32
  ${VIEWER_SOURCES}
  viewer.manifest
  ${CMAKE_SOURCE_DIR}/src/ui/rd_app.rc
)

add_dependencies(viewer viewer_bump_version)
# Ensure objs recompile after the bump rewrites product_version.h (same build).
set_source_files_properties(${VIEWER_SOURCES} PROPERTIES
  OBJECT_DEPENDS "${VIEWER_VERSION_STAMP}")

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
