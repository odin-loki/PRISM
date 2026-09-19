cmake_minimum_required(VERSION 3.20)
project(helix LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HELIX_CUDA "Build CUDA mutation kernels" ON)
option(HELIX_LLAMA "Build llama.cpp inference" ON)
option(HELIX_QT "Build native Qt GUI" ON)

# Header-only xsimd. Prefer a vendored copy (third_party/xsimd) so a MinGW
# configure does not need network; FetchContent is the fallback.
set(_helix_xsimd_inc "${CMAKE_SOURCE_DIR}/third_party/xsimd/include")
if(EXISTS "${_helix_xsimd_inc}/xsimd/xsimd.hpp")
  add_library(xsimd INTERFACE)
  add_library(xsimd::xsimd ALIAS xsimd)
  target_include_directories(xsimd INTERFACE "${_helix_xsimd_inc}")
  message(STATUS "xsimd: vendored headers at ${_helix_xsimd_inc}")
else()
  include(FetchContent)
  FetchContent_Declare(xsimd
    GIT_REPOSITORY https://github.com/xtensor-stack/xsimd.git
    GIT_TAG 13.2.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(xsimd)
endif()

add_library(helix_native SHARED
  native/src/hash.cpp
  native/src/havoc.cpp
  native/src/pymod.cpp
)
target_include_directories(helix_native PUBLIC native/include)
target_link_libraries(helix_native PUBLIC xsimd)
target_compile_definitions(helix_native PRIVATE HELIX_NATIVE_EXPORTS)

if(MINGW)
  # Produce helix_native.dll (not libhelix_native.dll) and avoid a
  # runtime dependency on Strawberry's libstdc++-6.dll.
  set_target_properties(helix_native PROPERTIES PREFIX "" OUTPUT_NAME "helix_native")
  target_link_options(helix_native PRIVATE -static-libgcc -static-libstdc++)
endif()

if(MSVC)
  target_compile_options(helix_native PRIVATE /O2 /arch:AVX2)
else()
  target_compile_options(helix_native PRIVATE -O3 -mavx2)
endif()

if(HELIX_CUDA)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    add_library(helix_cuda SHARED native/cuda/mutate.cu)
    target_include_directories(helix_cuda PUBLIC native/include)
    target_compile_features(helix_cuda PUBLIC cxx_std_20)
    target_link_libraries(helix_native PUBLIC helix_cuda)
    target_compile_definitions(helix_native PRIVATE HELIX_HAS_CUDA)
  else()
    message(WARNING "nvcc not usable with this host compiler — CUDA kernel skipped (NOTRUN, not a silent disable)")
  endif()
endif()

if(HELIX_LLAMA)
  include(FetchContent)
  FetchContent_Declare(llama
    GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git
    GIT_TAG b6875)
  set(GGML_CUDA ON CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_SERVER ON CACHE BOOL "" FORCE)
  # llama.cpp wants MSVC on Windows+CUDA. If the host is MinGW, skip.
  if(MSVC)
    FetchContent_MakeAvailable(llama)
    target_link_libraries(helix_native PUBLIC llama)
    target_compile_definitions(helix_native PRIVATE HELIX_HAS_LLAMA)
  else()
    message(WARNING "llama.cpp+CUDA on Windows needs MSVC. Python talks to Ollama (same GGUF) until then.")
  endif()
endif()

if(HELIX_QT)
  find_package(Qt6 COMPONENTS Widgets QUIET)
  if(Qt6_FOUND)
    qt_standard_project_setup()
    qt_add_executable(helix_gui
      native/gui/main.cpp
      native/gui/MainWindow.cpp
    )
    target_link_libraries(helix_gui PRIVATE Qt6::Widgets helix_native)
  else()
    message(WARNING "Qt6 not found — use python -m helix --gui (PySide6) which is the same Qt 6 widgets API.")
  endif()
endif()

add_executable(helix_cli native/src/cli.cpp)
target_link_libraries(helix_cli PRIVATE helix_native)
if(MINGW)
  target_link_options(helix_cli PRIVATE -static-libgcc -static-libstdc++)
endif()
