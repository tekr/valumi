idf_component_register(
    SRCS
        "main.c"
        "auth.c"
        "auth_core.c"
        "button_seq.c"
        "captive_dns.c"
        "captive_dns_core.c"
        "carousel.c"
        "chart_path.c"
        "code.c"
        "firmware.c"
        "local_time.c"
        "logo.c"
        "market.c"
        "market_core.c"
        "memwatch.c"
        "okx_client.c"
        "page_transition.c"
        "screens.c"
        "settings.c"
        "settings_json.c"
        "settings_store.c"
        "ui.c"
        "web_panel.c"
        "wifi_mgr.c"
        "wifi_policy.c"
        "wifi_qr.c"
        "fonts/font_large.c"
        "fonts/font_medium.c"
        "fonts/font_small.c"
    INCLUDE_DIRS "." "fonts"
    REQUIRES
        sh2lib
        board_c6_lcd147
        esp_timer
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_http_server
        esp-tls
        mbedtls
        json
        lwip
        app_update
        bootloader_support
)

# Development Wi-Fi seed (gitignored; see app_config.h). Globbed with
# CONFIGURE_DEPENDS so that creating or deleting the file triggers a
# reconfigure: a plain __has_include is invisible to the build system, and a
# file that appears after the first build would otherwise be silently ignored.
file(GLOB WIFI_SECRETS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/wifi_secrets.h")
if(WIFI_SECRETS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_HAVE_WIFI_SECRETS=1)
    set_source_files_properties(settings_store.c PROPERTIES OBJECT_DEPENDS "${WIFI_SECRETS}")
    message(STATUS "valumi: seeding first boot from wifi_secrets.h")
endif()

# The panel page is served gzipped: ~4x smaller in flash and on the air.
# Compressed at build time with a fixed mtime, so an unchanged page gives a
# byte-identical image.
set(WEB_PAGE "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")
set(WEB_PAGE_GZ "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
if(NOT EXISTS "${WEB_PAGE}")
    message(FATAL_ERROR "web panel page missing: ${WEB_PAGE}")
endif()
add_custom_command(
    OUTPUT "${WEB_PAGE_GZ}"
    COMMAND ${PYTHON} -c "import gzip,sys;d=open(sys.argv[1],'rb').read();open(sys.argv[2],'wb').write(gzip.compress(d,9,mtime=0))" "${WEB_PAGE}" "${WEB_PAGE_GZ}"
    DEPENDS "${WEB_PAGE}"
    VERBATIM)
add_custom_target(web_page_gz DEPENDS "${WEB_PAGE_GZ}")
add_dependencies(${COMPONENT_LIB} web_page_gz)
target_add_binary_data(${COMPONENT_LIB} "${WEB_PAGE_GZ}" BINARY)
# The logo the page shows, from tools/gen_logo.py.
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/web/logo.png" BINARY)
