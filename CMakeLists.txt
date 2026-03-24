cmake_minimum_required(VERSION 3.16)
project(geany-code VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GEANY REQUIRED geany)
pkg_check_modules(GTK REQUIRED gtk+-3.0)
pkg_check_modules(GTKSV REQUIRED gtksourceview-4)
pkg_check_modules(JSONGLIB REQUIRED json-glib-1.0)
pkg_check_modules(GIO REQUIRED gio-2.0)

# Determine where Geany loads plugins from
pkg_get_variable(GEANY_LIBDIR geany libdir)
set(GEANY_PLUGIN_DIR "${GEANY_LIBDIR}/geany" CACHE PATH "Geany plugin directory")

# Data directory for web assets
set(GEANY_CODE_DATA_DIR "${CMAKE_INSTALL_PREFIX}/share/geany-code"
    CACHE PATH "Data directory for web assets")

# MCP server binary path (for the plugin to reference)
set(GEANY_CODE_MCP_SERVER "${CMAKE_CURRENT_BINARY_DIR}/geany-code-mcp-server"
    CACHE PATH "Path to MCP server binary")

# Plugin shared library
add_library(geany-code SHARED
    src/plugin.c
    src/chat_widget.c
    src/chat_webview.c
    src/chat_input.c
    src/cli_session.c
    src/editor_bridge.c
    src/editor_dbus.c
    src/session_picker.c
    src/settings.c
)

set_target_properties(geany-code PROPERTIES PREFIX "")

target_include_directories(geany-code PRIVATE
    ${GEANY_INCLUDE_DIRS}
    ${GTK_INCLUDE_DIRS}
    ${GTKSV_INCLUDE_DIRS}
    ${JSONGLIB_INCLUDE_DIRS}
    ${GIO_INCLUDE_DIRS}
)

target_link_libraries(geany-code PRIVATE
    ${GEANY_LIBRARIES}
    ${GTK_LIBRARIES}
    ${GTKSV_LIBRARIES}
    ${JSONGLIB_LIBRARIES}
    ${GIO_LIBRARIES}
)

target_compile_definitions(geany-code PRIVATE
    GTK
    GEANY_CODE_DATA_DIR="${GEANY_CODE_DATA_DIR}"
    GEANY_CODE_VERSION="${PROJECT_VERSION}"
    GEANY_CODE_MCP_SERVER="${GEANY_CODE_MCP_SERVER}"
)

# MCP server binary
add_executable(geany-code-mcp-server
    src/mcp_server.c
)

target_include_directories(geany-code-mcp-server PRIVATE
    ${GIO_INCLUDE_DIRS}
    ${JSONGLIB_INCLUDE_DIRS}
)

target_link_libraries(geany-code-mcp-server PRIVATE
    ${GIO_LIBRARIES}
    ${JSONGLIB_LIBRARIES}
)

# Install plugin
install(TARGETS geany-code LIBRARY DESTINATION "${GEANY_PLUGIN_DIR}")

# Install MCP server
install(TARGETS geany-code-mcp-server RUNTIME DESTINATION bin)

# Web assets no longer needed (native GTK3 rendering)
