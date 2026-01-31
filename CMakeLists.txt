# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Get version from git on every build
execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(GIT_VERSION)
    set(PROJECT_VER ${GIT_VERSION})
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-wifi-bridge)
