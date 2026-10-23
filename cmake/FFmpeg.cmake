# Optional FFmpeg (libavformat, libavcodec, libavutil) for ProRes RAW and other
# RAW video in MOV/MP4 containers. Decoding ProRes RAW needs FFmpeg 8.0 or newer
# (FFmpeg 9.0 also linearizes it and reports its black/white levels); an older
# FFmpeg still builds and serves the other formats.
#
# Sets SERCONV_HAVE_FFMPEG and, when found, the INTERFACE target serconv_ffmpeg.
#
# Lookup order:
#   1. find_package(FFMPEG): the module vcpkg installs with its ffmpeg port,
#      which also lists the static dependencies (system libraries, frameworks).
#   2. pkg-config (libavformat libavcodec libavutil). Static archives are
#      linked with the --static flags, so a minimal static FFmpeg built with
#      tools/build-ffmpeg-minimal.sh works through PKG_CONFIG_PATH.
#
# With SERCONV_FFMPEG=AUTO a FFmpeg that is found but cannot be linked is
# left out with a warning, so a build always succeeds; with ON it is an error.

include(CheckCXXSourceCompiles)

set(SERCONV_HAVE_FFMPEG OFF)
set(SERCONV_FFMPEG_HOW "")

if(NOT SERCONV_FFMPEG STREQUAL "OFF")
  set(_ff_includes "")
  set(_ff_libraries "")
  set(_ff_link_dirs "")

  find_package(FFMPEG QUIET)
  if(FFMPEG_FOUND AND FFMPEG_INCLUDE_DIRS AND FFMPEG_LIBRARIES)
    set(_ff_includes ${FFMPEG_INCLUDE_DIRS})
    set(_ff_libraries ${FFMPEG_LIBRARIES})
    set(_ff_link_dirs ${FFMPEG_LIBRARY_DIRS})
    set(SERCONV_FFMPEG_HOW "find_package(FFMPEG)")
  else()
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
      pkg_check_modules(SERCONV_AV QUIET libavformat libavcodec libavutil)
      if(SERCONV_AV_FOUND)
        set(_ff_includes ${SERCONV_AV_INCLUDE_DIRS})
        if(SERCONV_AV_LINK_LIBRARIES MATCHES "\\.a(;|$)")
          # Static archives: link the private dependencies as well.
          set(_ff_libraries ${SERCONV_AV_STATIC_LDFLAGS})
          set(SERCONV_FFMPEG_HOW "pkg-config, static")
        else()
          set(_ff_libraries ${SERCONV_AV_LINK_LIBRARIES})
          set(SERCONV_FFMPEG_HOW "pkg-config")
        endif()
      endif()
    endif()
  endif()

  if(_ff_libraries)
    if(APPLE)
      # A static FFmpeg built for macOS uses these system frameworks.
      list(APPEND _ff_libraries "-framework CoreFoundation" "-framework CoreMedia" "-framework CoreVideo"
                                "-framework VideoToolbox" "-framework AudioToolbox" "-framework CoreServices"
                                "-framework Security")
    endif()

    # Make sure it links before relying on it.
    set(CMAKE_REQUIRED_INCLUDES ${_ff_includes})
    set(CMAKE_REQUIRED_LIBRARIES ${_ff_libraries})
    if(_ff_link_dirs)
      set(CMAKE_REQUIRED_LINK_OPTIONS "")
      foreach(_d IN LISTS _ff_link_dirs)
        if(MSVC)
          list(APPEND CMAKE_REQUIRED_LINK_OPTIONS "/LIBPATH:${_d}")
        else()
          list(APPEND CMAKE_REQUIRED_LINK_OPTIONS "-L${_d}")
        endif()
      endforeach()
    endif()
    find_package(Threads)
    if(TARGET Threads::Threads AND NOT MSVC)
      list(APPEND CMAKE_REQUIRED_LIBRARIES Threads::Threads)
    endif()
    check_cxx_source_compiles("
      extern \"C\" {
      #include <libavcodec/avcodec.h>
      #include <libavformat/avformat.h>
      }
      int main() {
        AVFormatContext* f = avformat_alloc_context();
        const AVCodec* c = avcodec_find_decoder_by_name(\"prores_raw\");
        avformat_free_context(f);
        return c ? 0 : 1;
      }" SERCONV_FFMPEG_LINKS)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)

    if(SERCONV_FFMPEG_LINKS)
      add_library(serconv_ffmpeg INTERFACE)
      target_include_directories(serconv_ffmpeg INTERFACE ${_ff_includes})
      if(_ff_link_dirs)
        target_link_directories(serconv_ffmpeg INTERFACE ${_ff_link_dirs})
      endif()
      target_link_libraries(serconv_ffmpeg INTERFACE ${_ff_libraries})
      set(SERCONV_HAVE_FFMPEG ON)
    elseif(SERCONV_FFMPEG STREQUAL "ON")
      message(FATAL_ERROR "FFmpeg was found (${SERCONV_FFMPEG_HOW}) but a test program does not link with it")
    else()
      message(WARNING "FFmpeg was found (${SERCONV_FFMPEG_HOW}) but a test program does not link with it; "
                      "building without the FFmpeg reader (no ProRes RAW)")
    endif()
  endif()

  if(SERCONV_HAVE_FFMPEG)
    message(STATUS "FFmpeg reader: enabled (${SERCONV_FFMPEG_HOW})")
  elseif(SERCONV_FFMPEG STREQUAL "ON")
    message(FATAL_ERROR "SERCONV_FFMPEG=ON but FFmpeg (libavformat, libavcodec, libavutil) was not found")
  else()
    message(STATUS "FFmpeg reader: disabled (FFmpeg not found or not usable); ProRes RAW will not be available")
  endif()
endif()
