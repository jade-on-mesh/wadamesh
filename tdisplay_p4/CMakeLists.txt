cmake_minimum_required(VERSION 3.10)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(application)

# Flash: the 4 MB OTA slot is nearly full, and every component builds at -O2 for
# the panel (sdkconfigs/tdisplay_p4: CONFIG_COMPILER_OPTIMIZATION_PERF, #167).
# The pixel path (lvgl, esp_lcd, variants/tdisplay_p4/) and the radio and mesh
# crypto (RadioLib, meshcore) stay -O2; these, none of them on it, go to -Os.
foreach(_c lwip bt espressif__arduino-esp32 ardlibs nicolaielectronics__esp-hosted-tanmatsu
           joltwallet__littlefs fatfs nvs_flash h2zero__esp-nimble-cpp espressif__esp_wifi_remote
           esp_netif esp_http_client sdmmc spiffs console)
  if(TARGET __idf_${_c})
    target_compile_options(__idf_${_c} PRIVATE -Os)
  endif()
endforeach()
foreach(_t mbedtls mbedcrypto mbedx509)
  if(TARGET ${_t})
    target_compile_options(${_t} PRIVATE -Os)
  endif()
endforeach()
