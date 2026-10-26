cmake_minimum_required(VERSION 3.16.0)
# Fixed app version so the build does not depend on `git describe`.
set(PROJECT_VER "0.1.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sand_physics)
