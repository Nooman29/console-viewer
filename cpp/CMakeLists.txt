cmake_minimum_required(VERSION 3.16)
project(ConsoleViewer VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive-)
else()
  add_compile_options(-Wall -Wextra)
endif()

# ---- Core: plain C++, no Windows. Builds and tests anywhere. ----
add_library(cv_core STATIC
  src/core/rules.cpp
  src/core/controller.cpp
)
target_include_directories(cv_core PUBLIC src)

enable_testing()
add_executable(cv_tests tests/test_core.cpp)
target_link_libraries(cv_tests PRIVATE cv_core)
add_test(NAME core COMMAND cv_tests)

# ---- The app: Windows only (Media Foundation, WASAPI, Win32). ----
if(WIN32)
  add_executable(console-viewer WIN32
    src/win/main.cpp
    src/win/capture.cpp
    src/win/audio.cpp
    src/win/devices.cpp
    src/win/window.cpp
    src/win/screenshot.cpp
  )
  target_compile_definitions(console-viewer PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    CV_VERSION="${PROJECT_VERSION}"
  )
  target_link_libraries(console-viewer PRIVATE
    cv_core
    mfplat mf mfreadwrite mfuuid
    ole32 oleaut32 uuid propsys
    user32 gdi32 shell32 winmm windowscodecs
  )
  if(MINGW)
    # One file to hand to someone: no extra DLLs to ship.
    target_link_options(console-viewer PRIVATE -static -municode)
    target_link_options(cv_tests PRIVATE -static)
  endif()
endif()
