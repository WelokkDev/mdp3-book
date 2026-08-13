# Warnings-as-errors, applied per target rather than globally so that
# FetchContent dependencies (GoogleTest) are never compiled under our flags.

function(bookreplay_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4
      /permissive-      # conforming mode; two-phase lookup, no MS extensions
      /utf-8
      /Zc:__cplusplus   # without this __cplusplus reports C++98
      /Zc:preprocessor  # conforming preprocessor
      /EHsc
    )
    if(BOOKREPLAY_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wconversion       # int64 fixed-point prices; a silent narrowing here is
      -Wsign-conversion  # a wrong price, not a style issue
      -Wcast-align
      -Wnull-dereference
      -Wdouble-promotion
      -Wimplicit-fallthrough
      -Wformat=2
      -Wunused
    )
    if(BOOKREPLAY_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
