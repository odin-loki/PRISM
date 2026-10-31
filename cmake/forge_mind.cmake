# FORGE mind: PRISM's only language-model engine (replaces llama.cpp, retired).
# FORGE is mandatory and unpinned: it tracks odin-loki/FORGE main and is never vendored.
# The cohesion block (log/exec/pool/cache) may already have set FORGE_SOURCE_DIR.
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
if(NOT EXISTS "${FORGE_SOURCE_DIR}/mind/include/forge/mind/mind.hpp")
  message(FATAL_ERROR "PRISM requires FORGE (odin-loki/FORGE main) for its AI engine; "
                      "mind/ not found under '${FORGE_SOURCE_DIR}'. Set -DFORGE_SOURCE_DIR=/path/to/FORGE.")
endif()
# FORGE's own build provides forge_mind / forge_verify / forge_lane_mv. Tests, apps and
# examples are off; only the targets PRISM links get built (EXCLUDE_FROM_ALL).
set(FORGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FORGE_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(FORGE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FORGE_JNI OFF CACHE BOOL "" FORCE)
add_subdirectory("${FORGE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_forge" EXCLUDE_FROM_ALL)
foreach(_t forge_mind forge_verify forge_lane_mv)
  if(NOT TARGET ${_t})
    message(FATAL_ERROR "FORGE at ${FORGE_SOURCE_DIR} has no target ${_t} (FORGE main changed?)")
  endif()
endforeach()
add_library(prism_forge_mind INTERFACE)
target_link_libraries(prism_forge_mind INTERFACE forge_mind forge_verify forge_lane_mv)
message(STATUS "PRISM: AI engine = FORGE mind from ${FORGE_SOURCE_DIR}")
