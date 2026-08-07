set(AGENT_SOURCES
  main.cpp
  log.cpp
  gateway_config.cpp
  gateway_client.cpp
  debug_http.cpp
  audit_client.cpp
  process_audit.cpp
  service_host.cpp
  session_monitor.cpp
  host_inventory.cpp
)

# When any agent or media source changes, bump ROAD_DESK_VERSION_PATCH before compiling.
set(AGENT_VERSION_STAMP "${CMAKE_BINARY_DIR}/generated/agent_version.stamp")
add_custom_command(
  OUTPUT "${AGENT_VERSION_STAMP}"
  COMMAND "${ROAD_DESK_PWSH}" -NoProfile -ExecutionPolicy Bypass
          -File "${CMAKE_SOURCE_DIR}/scripts/bump_product_version.ps1"
          -Stamp "${AGENT_VERSION_STAMP}"
  DEPENDS
    ${AGENT_SOURCES}
    host-agent.manifest
    "${CMAKE_SOURCE_DIR}/src/ui/rd_app.rc"
    "${CMAKE_SOURCE_DIR}/src/media/mux_host.cpp"
    "${CMAKE_SOURCE_DIR}/src/media/mux_client.cpp"
    "${CMAKE_SOURCE_DIR}/src/media/mux_protocol.h"
    "${CMAKE_SOURCE_DIR}/scripts/bump_product_version.ps1"
  COMMENT "Bump product version (agent or media sources changed)"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  VERBATIM
)
add_custom_target(agent_bump_version DEPENDS "${AGENT_VERSION_STAMP}")

add_executable(host-agent
  ${AGENT_SOURCES}
  host-agent.manifest
  ${CMAKE_SOURCE_DIR}/src/ui/rd_app.rc
)

add_dependencies(host-agent agent_bump_version)
set_source_files_properties(${AGENT_SOURCES} PROPERTIES
  OBJECT_DEPENDS "${AGENT_VERSION_STAMP}")

target_include_directories(host-agent PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(host-agent PRIVATE
  road_desk_media_host
  road_desk_session
  road_desk_common
  winhttp
  iphlpapi
  ws2_32
  ole32
  comdlg32
  shell32
  gdiplus
  comctl32
  advapi32
  wtsapi32
)

set_target_properties(host-agent PROPERTIES OUTPUT_NAME "host-agent")

# Mirror Attach.ToDesktop scrub needs HKLM write — always elevate via UAC.
if(MSVC)
  target_link_options(host-agent PRIVATE
      "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
  )
  # Keep a full manifest beside sources for documentation / alternate toolchains.
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/host-agent.manifest"
    "${CMAKE_CURRENT_BINARY_DIR}/host-agent.manifest"
    COPYONLY
  )
endif()
