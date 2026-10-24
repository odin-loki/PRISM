# Python-free integration for vendored Z3 (CPP_PORT_PLAN phase 6).
# Upstream third_party/z3/CMakeLists.txt is unchanged; PRISM bootstraps z3gen
# and points Python3_EXECUTABLE at it so Ninja never invokes Python for codegen.
# Pre-generated outputs live in cmake/z3-generated/ (refresh after bumping the
# z3 pin: one-off upstream Z3 build with real Python, copy outputs into
# cmake/z3-generated/, update tree_sha256; see docs/CPP_PORT_PLAN.md phase 6).

if(NOT PRISM_Z3)
  return()
endif()

set(_prism_z3_src "${PRISM_ROOT}/third_party/z3")
if(NOT EXISTS "${_prism_z3_src}/CMakeLists.txt")
  return()
endif()

set(_prism_z3_gen "${PRISM_ROOT}/cmake/z3-generated")
set(_prism_z3_digest_file "${_prism_z3_gen}/tree_sha256")
if(NOT EXISTS "${_prism_z3_digest_file}")
  message(FATAL_ERROR "cmake/z3-generated/tree_sha256 missing (refresh cmake/z3-generated/ after a z3 pin bump)")
endif()
file(READ "${_prism_z3_digest_file}" _prism_z3_gen_digest)
string(STRIP "${_prism_z3_gen_digest}" _prism_z3_gen_digest)

# Must match third_party/MANIFEST.toml linked:z3 tree_sha256.
set(_prism_manifest "${PRISM_ROOT}/third_party/MANIFEST.toml")
file(READ "${_prism_manifest}" _prism_manifest_text)
string(REGEX MATCH "name = \"z3\"[^\[]*tree_sha256 = \"([0-9a-f]+)\"" _z3_pin "${_prism_manifest_text}")
if(NOT CMAKE_MATCH_1)
  message(FATAL_ERROR "could not read z3 tree_sha256 from MANIFEST.toml")
endif()
if(NOT CMAKE_MATCH_1 STREQUAL _prism_z3_gen_digest)
  message(FATAL_ERROR
    "cmake/z3-generated/tree_sha256 (${_prism_z3_gen_digest}) "
    "does not match MANIFEST.toml z3 pin (${CMAKE_MATCH_1})")
endif()

# Bootstrap z3gen (std-only; compiled once per build directory).
set(_prism_z3gen_dir "${CMAKE_BINARY_DIR}/z3gen-bootstrap")
file(MAKE_DIRECTORY "${_prism_z3gen_dir}")
set(_prism_z3gen "${_prism_z3gen_dir}/z3gen${CMAKE_EXECUTABLE_SUFFIX}")
set(_prism_z3gen_src "${PRISM_ROOT}/src/tools/z3gen/main.cpp")
set(_prism_z3gen_stamp "${_prism_z3gen_dir}/z3gen.stamp")
set(_prism_z3gen_inputs "${_prism_z3gen_src}" "${_prism_z3_digest_file}")
set(_prism_z3gen_needs_build TRUE)
if(EXISTS "${_prism_z3gen_stamp}")
  file(READ "${_prism_z3gen_stamp}" _prism_z3gen_old)
  string(STRIP "${_prism_z3gen_old}" _prism_z3gen_old)
  if(_prism_z3gen_old STREQUAL "${_prism_z3gen_inputs}" AND EXISTS "${_prism_z3gen}")
    set(_prism_z3gen_needs_build FALSE)
  endif()
endif()
if(_prism_z3gen_needs_build)
  if(MSVC)
    set(_prism_z3gen_flags /std:c++latest /O2 /EHsc /W3)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
      set(_prism_z3gen_out -o "${_prism_z3gen}")
    else()
      set(_prism_z3gen_out "/Fe:${_prism_z3gen}")
    endif()
  else()
    set(_prism_z3gen_flags -std=c++23 -O2)
    set(_prism_z3gen_out -o "${_prism_z3gen}")
  endif()
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "PRISM_Z3_GENERATED_ROOT=${_prism_z3_gen}"
            "${CMAKE_CXX_COMPILER}" ${_prism_z3gen_flags}
            "${_prism_z3gen_src}" ${_prism_z3gen_out}
    RESULT_VARIABLE _prism_z3gen_rc
    ERROR_VARIABLE _prism_z3gen_err
    OUTPUT_VARIABLE _prism_z3gen_out)
  if(_prism_z3gen_rc OR NOT EXISTS "${_prism_z3gen}")
    message(FATAL_ERROR "z3gen bootstrap failed (${_prism_z3gen_rc}): ${_prism_z3gen_err} ${_prism_z3gen_out}")
  endif()
  file(WRITE "${_prism_z3gen_stamp}" "${_prism_z3gen_inputs}")
endif()
file(WRITE "${_prism_z3gen_dir}/z3gen_root.txt" "${_prism_z3_gen}")

# Z3 build steps invoke Python3_EXECUTABLE without our configure-time env; wrap z3gen.
if(CMAKE_HOST_UNIX)
  set(_prism_z3gen_shim "${_prism_z3gen_dir}/z3gen_shim")
  file(WRITE "${_prism_z3gen_shim}"
    "#!/bin/sh\nexport PRISM_Z3_GENERATED_ROOT=\"${_prism_z3_gen}\"\nexec \"${_prism_z3gen}\" \"$@\"\n")
  execute_process(COMMAND chmod +x "${_prism_z3gen_shim}")
  set(_prism_python3 "${_prism_z3gen_shim}")
else()
  # FindPython3 on Windows probes a .exe interpreter; z3gen reads z3gen_root.txt beside the binary.
  set(_prism_python3 "${_prism_z3gen}")
endif()

set(Python3_EXECUTABLE "${_prism_python3}" CACHE FILEPATH "PRISM z3gen (no Python for Z3 codegen)" FORCE)
set(Python3_FOUND TRUE CACHE BOOL "" FORCE)
set(Python3_VERSION "3.11.0" CACHE STRING "" FORCE)
set(Python3_VERSION_MAJOR 3 CACHE STRING "" FORCE)
set(Python3_VERSION_MINOR 11 CACHE STRING "" FORCE)
set(Python3_VERSION_PATCH 0 CACHE STRING "" FORCE)

set(Z3_BUILD_LIBZ3_SHARED OFF CACHE BOOL "" FORCE)
set(Z3_BUILD_EXECUTABLE OFF CACHE BOOL "" FORCE)
set(Z3_BUILD_TEST_EXECUTABLES OFF CACHE BOOL "" FORCE)
set(Z3_ENABLE_EXAMPLE_TARGETS OFF CACHE BOOL "" FORCE)
set(Z3_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(Z3_BUILD_PYTHON_BINDINGS OFF CACHE BOOL "" FORCE)
set(Z3_INSTALL_PYTHON_BINDINGS OFF CACHE BOOL "" FORCE)
set(Z3_BUILD_DOTNET_BINDINGS OFF CACHE BOOL "" FORCE)
set(Z3_INSTALL_DOTNET_BINDINGS OFF CACHE BOOL "" FORCE)
set(Z3_BUILD_JAVA_BINDINGS OFF CACHE BOOL "" FORCE)
set(Z3_INSTALL_JAVA_BINDINGS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/z3 EXCLUDE_FROM_ALL)
set(PRISM_HAS_Z3 ON)
