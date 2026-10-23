# Builds LibRaw from source as a static library: target serconv_libraw.
#
# serconv does not use a system or vcpkg LibRaw on purpose. Stock LibRaw
# refuses CR3/CRM files larger than 2 GB (LIBRAW_MAX_CR3_RAW_FILE_SIZE), and a
# Canon Cinema RAW Light clip passes that size within seconds. LibRaw's CR3
# parser itself uses 64-bit offsets (co64 chunk tables), so the limit is only
# raised here, nothing else is changed.
#
#   -DSERCONV_LIBRAW_SOURCE_DIR=/path/to/LibRaw   use a local source tree
#   -DSERCONV_LIBRAW_VERSION=0.22.2               release tag to download

set(SERCONV_LIBRAW_VERSION "0.22.2" CACHE STRING "LibRaw release tag to download")
set(SERCONV_LIBRAW_SOURCE_DIR "" CACHE PATH "Local LibRaw source tree (instead of downloading)")

if(SERCONV_LIBRAW_SOURCE_DIR)
  set(_serconv_libraw_dir "${SERCONV_LIBRAW_SOURCE_DIR}")
else()
  include(FetchContent)
  FetchContent_Declare(serconv_libraw_src
    GIT_REPOSITORY https://github.com/LibRaw/LibRaw.git
    GIT_TAG        ${SERCONV_LIBRAW_VERSION}
    GIT_SHALLOW    TRUE
  )
  # LibRaw has no top-level CMakeLists.txt, so this only downloads it.
  FetchContent_MakeAvailable(serconv_libraw_src)
  set(_serconv_libraw_dir "${serconv_libraw_src_SOURCE_DIR}")
endif()

if(NOT EXISTS "${_serconv_libraw_dir}/libraw/libraw.h")
  message(FATAL_ERROR "LibRaw sources not found in '${_serconv_libraw_dir}'")
endif()

file(STRINGS "${_serconv_libraw_dir}/libraw/libraw_version.h" _lr_lines
     REGEX "^#define LIBRAW_(MAJOR|MINOR|PATCH)_VERSION")
foreach(_l IN LISTS _lr_lines)
  if(_l MATCHES "LIBRAW_MAJOR_VERSION ([0-9]+)")
    set(_lr_major ${CMAKE_MATCH_1})
  elseif(_l MATCHES "LIBRAW_MINOR_VERSION ([0-9]+)")
    set(_lr_minor ${CMAKE_MATCH_1})
  elseif(_l MATCHES "LIBRAW_PATCH_VERSION ([0-9]+)")
    set(_lr_patch ${CMAKE_MATCH_1})
  endif()
endforeach()
set(SERCONV_LIBRAW_VERSION_FOUND "${_lr_major}.${_lr_minor}.${_lr_patch}")

# The *_ph.cpp files are placeholders that LibRaw's own makefiles do not build.
file(GLOB_RECURSE _serconv_libraw_sources CONFIGURE_DEPENDS "${_serconv_libraw_dir}/src/*.cpp")
list(FILTER _serconv_libraw_sources EXCLUDE REGEX "_ph\\.cpp$")

add_library(serconv_libraw STATIC ${_serconv_libraw_sources})
target_include_directories(serconv_libraw SYSTEM PUBLIC "${_serconv_libraw_dir}")
# _USE_MATH_DEFINES: LibRaw includes <math.h> before defining it itself, and
# MSVC and MinGW only provide M_PI with it.
target_compile_definitions(serconv_libraw
  PUBLIC  LIBRAW_NODLL
  PRIVATE "LIBRAW_MAX_CR3_RAW_FILE_SIZE=9223372036854775807LL" _USE_MATH_DEFINES)
set_target_properties(serconv_libraw PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  target_compile_definitions(serconv_libraw PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
  target_compile_options(serconv_libraw PRIVATE /w /bigobj /utf-8)
else()
  target_compile_options(serconv_libraw PRIVATE -w)
endif()

find_package(Threads REQUIRED)
target_link_libraries(serconv_libraw PUBLIC Threads::Threads)
if(WIN32)
  # LibRaw uses the Winsock byte-order functions on Windows.
  target_link_libraries(serconv_libraw PUBLIC ws2_32)
endif()
