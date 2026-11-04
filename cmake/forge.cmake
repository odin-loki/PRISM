# FORGE: PRISM's one platform layer (log, exec, pool, cache, clock, settings, OS,
# memory, intrinsics) and its only language-model engine (llama.cpp retired).
# Mandatory and unpinned: tracks odin-loki/FORGE main, never vendored.
# This is the single FORGE integration point; nothing else locates FORGE.
set(FORGE_SOURCE_DIR "" CACHE PATH "FORGE checkout (empty: fetch odin-loki/FORGE main)")
if(NOT FORGE_SOURCE_DIR)
  include(FetchContent)
  FetchContent_Declare(forge GIT_REPOSITORY https://github.com/odin-loki/FORGE.git GIT_TAG main GIT_SHALLOW TRUE)
  FetchContent_GetProperties(forge)
  if(NOT forge_POPULATED)
    if(POLICY CMP0169)
      cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(forge)
  endif()
  set(FORGE_SOURCE_DIR "${forge_SOURCE_DIR}" CACHE PATH "FORGE checkout (empty: fetch odin-loki/FORGE main)" FORCE)
endif()
foreach(_req core/include/forge/core/log.hpp os/include/forge/os/os.hpp mind/include/forge/mind/mind.hpp)
  if(NOT EXISTS "${FORGE_SOURCE_DIR}/${_req}")
    message(FATAL_ERROR "PRISM requires FORGE (odin-loki/FORGE main); '${_req}' not found under "
                        "'${FORGE_SOURCE_DIR}'. Set -DFORGE_SOURCE_DIR=/path/to/FORGE.")
  endif()
endforeach()

# PRISM_DEPS_ONLY configures the std-only prism-deps tool: headers only, no FORGE build.
if(NOT PRISM_DEPS_ONLY)
  # FORGE builds as a sub-project (PROJECT_SOURCE_DIR-relative since FORGE subproject-build).
  set(FORGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FORGE_BUILD_APPS OFF CACHE BOOL "" FORCE)
  set(FORGE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(FORGE_JNI OFF CACHE BOOL "" FORCE)
  add_subdirectory("${FORGE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_forge" EXCLUDE_FROM_ALL)
  foreach(_t forge_core forge_os forge_mind forge_verify forge_lane_mv)
    if(NOT TARGET ${_t})
      message(FATAL_ERROR "FORGE at ${FORGE_SOURCE_DIR} has no target ${_t} (FORGE main changed?)")
    endif()
  endforeach()
endif()

# prism_forge: runtime services (header modules) for every PRISM library and tool.
add_library(prism_forge INTERFACE)
target_include_directories(prism_forge SYSTEM INTERFACE
  "${FORGE_SOURCE_DIR}/core/include" "${FORGE_SOURCE_DIR}/os/include" "${FORGE_SOURCE_DIR}/support/include")
find_package(Threads REQUIRED)
target_link_libraries(prism_forge INTERFACE Threads::Threads)
if(WIN32)
  target_link_libraries(prism_forge INTERFACE ws2_32 bcrypt)
elseif(UNIX AND NOT APPLE)
  target_link_libraries(prism_forge INTERFACE ${CMAKE_DL_LIBS})
endif()
if(NOT PRISM_DEPS_ONLY)
  target_link_libraries(prism_forge INTERFACE forge_core forge_os)
  # prism_forge_mind: the AI engine (FORGE mind + verify + lane MV kernels).
  add_library(prism_forge_mind INTERFACE)
  target_link_libraries(prism_forge_mind INTERFACE prism_forge forge_mind forge_verify forge_lane_mv)
endif()
message(STATUS "PRISM: FORGE (mandatory) from ${FORGE_SOURCE_DIR}")
