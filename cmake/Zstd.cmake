# zstd is the only third-party dependency of the shipping library. However it
# arrives, it is normalized into one target: bookreplay::zstd.
#
# The default fetches a hash-pinned release tarball rather than a git tag,
# because a tag can be moved and a hash cannot. Distributions and CI images
# that already carry zstd can opt out with BOOKREPLAY_USE_SYSTEM_ZSTD.

option(BOOKREPLAY_USE_SYSTEM_ZSTD "Link an installed zstd instead of fetching a pinned one" OFF)

if(BOOKREPLAY_USE_SYSTEM_ZSTD)
  find_package(zstd CONFIG QUIET)
  if(TARGET zstd::libzstd_static)
    add_library(bookreplay::zstd ALIAS zstd::libzstd_static)
    return()
  elseif(TARGET zstd::libzstd_shared)
    add_library(bookreplay::zstd ALIAS zstd::libzstd_shared)
    return()
  endif()

  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(PC_ZSTD QUIET IMPORTED_TARGET libzstd)
    if(TARGET PkgConfig::PC_ZSTD)
      add_library(bookreplay::zstd ALIAS PkgConfig::PC_ZSTD)
      return()
    endif()
  endif()

  message(FATAL_ERROR
    "BOOKREPLAY_USE_SYSTEM_ZSTD is ON but no installed zstd was found. "
    "Install libzstd development files, or turn the option off to fetch a pinned copy.")
endif()

include(FetchContent)

FetchContent_Declare(zstd
  URL      https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
  URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
  # The tarball roots its CMake build in a subdirectory rather than at the top.
  SOURCE_SUBDIR build/cmake
  SYSTEM                       # keep our -Werror off zstd's headers
)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
# Nothing here compresses on a background thread, and disabling it keeps
# pthread off the link line.
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(zstd)

add_library(bookreplay::zstd ALIAS libzstd_static)
