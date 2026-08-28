# pybind11 backs the optional Python extension only; it is never linked into
# the shipping library.
#
# pybind11 publishes no release assets, so this pins GitHub's generated source
# archive rather than an uploaded tarball. The hash was taken from two
# independent downloads of v3.0.4.

option(BOOKREPLAY_USE_SYSTEM_PYBIND11 "Use an installed pybind11 instead of fetching a pinned one" OFF)

if(BOOKREPLAY_USE_SYSTEM_PYBIND11)
  find_package(pybind11 CONFIG QUIET)
  if(TARGET pybind11::module)
    return()
  endif()

  message(FATAL_ERROR
    "BOOKREPLAY_USE_SYSTEM_PYBIND11 is ON but no installed pybind11 was found. "
    "pip install pybind11 and pass -Dpybind11_DIR=$(python -m pybind11 --cmakedir), "
    "or turn the option off to fetch a pinned copy.")
endif()

include(FetchContent)

FetchContent_Declare(pybind11
  URL      https://github.com/pybind/pybind11/archive/refs/tags/v3.0.4.tar.gz
  URL_HASH SHA256=74b6a2c2b4573a400cafb6ecbf60c98df300cd3d0041296b913d02b2cbbb2676
  SYSTEM                       # keep our -Werror off pybind11's headers
)

set(PYBIND11_FINDPYTHON ON CACHE BOOL "" FORCE)
set(PYBIND11_TEST OFF CACHE BOOL "" FORCE)
set(PYBIND11_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(pybind11)
