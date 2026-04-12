# DAVEY JONES — top-level ESP-IDF project file.
# Arduino Nesso N1 pentesting firmware (ESP32-C6 + SX1262 LoRa).

cmake_minimum_required(VERSION 3.16)

# Pull in ESP-IDF's CMake functions + toolchain.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Point the build at our in-tree components/.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

# Lock the target to ESP32-C6 — the only chip we support.
set(IDF_TARGET "esp32c6")

project(davey_jones
    VERSION      0.1.0
    LANGUAGES    C
)
