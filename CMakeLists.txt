cmake_minimum_required(VERSION 3.16.0)
# The firmware version comes from version.txt (ESP-IDF reads it when
# PROJECT_VER is not set): one place, no dependency on git, so a clean
# checkout or a source archive builds with the same version.
#
# The release environment (platformio.ini, -DRELEASE_BUILD=1) adds
# sdkconfig.defaults.release on top of sdkconfig.defaults.
if(RELEASE_BUILD)
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.release")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sand_physics)
