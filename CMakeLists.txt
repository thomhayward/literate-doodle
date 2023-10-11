# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# initalize pico_sdk from installed location
# (note this can come from environment, CMake cache etc)
# set(PICO_SDK_PATH "/home/mathew/src/pico/pico-sdk")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(pico-bme280 C CXX ASM)

set(PICO_CXX_ENABLE_EXCEPTIONS 1)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(pico-bme280 pico_bme280.cc bme280.cc bme280_user.cc)

pico_set_program_name(pico-bme280 "pico-bme280")
pico_set_program_version(pico-bme280 "0.1")

add_compile_definitions(SPI_PORT=spi1)
add_compile_definitions(SPI_RX=12)
add_compile_definitions(SPI_CS=13)
add_compile_definitions(SPI_SCK=14)
add_compile_definitions(SPI_TX=15)
add_compile_definitions(ALTITUDE_AT_LOC=65)

add_compile_definitions(BME280_32BIT_ENABLE)

set(WIFI_SSID "${WIFI_SSID}" CACHE INTERNAL "WiFi SSID")
set(WIFI_PASSWORD "${WIFI_PASSWORD}" CACHE INTERNAL "WiFi password")
set(MQTT_SERVER_HOST "${MQTT_SERVER_HOST}" CACHE INTERNAL "MQTT Server Host")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(CMAKE_VERBOSE_MAKEFILE 1)
  add_compile_definitions(DEBUG)
  add_compile_definitions(UPDATE_INTERVAL=10)
else()
  add_compile_definitions(UPDATE_INTERVAL=60)
endif()

pico_enable_stdio_uart(pico-bme280 1)
pico_enable_stdio_usb(pico-bme280 0)

target_include_directories(pico-bme280 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/.. # for our common lwipopts
)

target_compile_definitions(pico-bme280 PRIVATE
  WIFI_SSID=\"${WIFI_SSID}\"
  WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
  MQTT_SERVER_HOST=\"${MQTT_SERVER_HOST}\"
  MQTT_SERVER_PORT=1883
)

target_link_libraries(pico-bme280
  pico_stdlib
  pico_cyw43_arch_lwip_poll
  pico_lwip_mqtt
  pico_lwip_mbedtls
  pico_mbedtls
  hardware_spi
  hardware_clocks
)

pico_add_extra_outputs(pico-bme280)
