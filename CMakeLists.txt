cmake_minimum_required(VERSION 3.16)

project(serconv VERSION 0.1.0
        DESCRIPTION "Converts cinema and mirrorless RAW video to SER for planetary imaging"
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SERCONV_BUILD_TESTS "Build the test programs" ON)
set(SERCONV_FFMPEG AUTO CACHE STRING
    "FFmpeg reader for ProRes RAW and other RAW video in MOV/MP4: AUTO, ON or OFF")
set_property(CACHE SERCONV_FFMPEG PROPERTY STRINGS AUTO ON OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LibRaw)   # target serconv_libraw
include(FFmpeg)   # SERCONV_HAVE_FFMPEG, target serconv_ffmpeg

find_package(Threads REQUIRED)

configure_file(src/version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp" @ONLY)

# Everything except main() is in a library so the tests can use it.
add_library(serconv_core STATIC
  src/util.cpp
  src/timeutil.cpp
  src/frame.cpp
  src/ser_file.cpp
  src/lj92.cpp
  src/bmff.cpp
  src/tiff_meta.cpp
  src/mlv_reader.cpp
  src/libraw_reader.cpp
  src/inputs.cpp
  src/process.cpp
  src/sidecar.cpp
  src/convert.cpp
  src/info.cpp
  src/options.cpp
)
target_include_directories(serconv_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(serconv_core PUBLIC serconv_libraw Threads::Threads)

if(SERCONV_HAVE_FFMPEG)
  target_sources(serconv_core PRIVATE src/ffmpeg_reader.cpp)
  target_compile_definitions(serconv_core PUBLIC SERCONV_HAVE_FFMPEG=1)
  target_link_libraries(serconv_core PUBLIC serconv_ffmpeg)
endif()

if(MSVC)
  target_compile_options(serconv_core PRIVATE /W3 /utf-8)
  target_compile_definitions(serconv_core PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(serconv_core PRIVATE -Wall -Wextra)
endif()

add_executable(serconv src/main.cpp)
target_link_libraries(serconv PRIVATE serconv_core)
if(MSVC)
  target_compile_options(serconv PRIVATE /utf-8)
endif()
if(WIN32)
  target_link_libraries(serconv PRIVATE shell32)  # CommandLineToArgvW
endif()

install(TARGETS serconv RUNTIME DESTINATION bin)

if(SERCONV_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "serconv ${PROJECT_VERSION}: LibRaw ${SERCONV_LIBRAW_VERSION_FOUND}, "
               "FFmpeg reader ${SERCONV_HAVE_FFMPEG}")
