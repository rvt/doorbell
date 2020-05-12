cmake_minimum_required(VERSION 3.0)

project(cmake_test)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_BUILD_TYPE Debug)

# Prepare "Catch" library for other executables
set(CATCH_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
add_library(Catch INTERFACE)
target_include_directories(Catch INTERFACE ${CATCH_INCLUDE_DIR})

ADD_DEFINITIONS(-DUNIT_TEST)

set(LIB_SOURCES
    ../.pio/libdeps/wemos/opt-parser/src/optparser.cpp
    ../lib/utils/digitalknob.cpp
    ../lib/utils/propertyutils.cpp
    ../lib/utils/utils.cpp
    ../lib/utils/makestring.cpp
)

set(LIB_HEADERS
    stubs
    ../.pio/libdeps/wemos/opt-parser/src
    ../lib/utils
)

include_directories(catch2 ${LIB_HEADERS})

# Make test executable
add_executable(tests main.cpp src/arduinostubs.hpp ${LIB_SOURCES})
target_link_libraries(tests Catch)
