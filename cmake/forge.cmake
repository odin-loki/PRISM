# FORGE: PRISM's one platform layer (log, exec, pool, cache, clock, settings, OS,
# memory, intrinsics) and its only language-model engine (llama.cpp retired).
# Mandatory, never vendored, pinned to one odin-loki/FORGE commit: the full SHA
# in cmake/FORGE_PIN. That file is the single source of truth; every workflow
# reads it for its FORGE checkout. Bump it with scripts/bump_forge_pin.sh in a
# reviewed one-line PR (docs/SUPPLY_CHAIN.md "FORGE").
# This is the single FORGE integration point; nothing else locates FORGE.
set(FORGE_GIT_URL "https://github.com/odin-loki/FORGE.git")
set(FORGE_PIN_FILE "${CMAKE_CURRENT_LIST_DIR}/FORGE_PIN")
file(READ "${FORGE_PIN_FILE}" FORGE_PIN)
string(STRIP "${FORGE_PIN}" FORGE_PIN)
string(LENGTH "${FORGE_PIN}" _forge_pin_len)
if(NOT _forge_pin_len EQUAL 40 OR NOT FORGE_PIN MATCHES "^[0-9a-f]+$")
  message(FATAL_ERROR "${FORGE_PIN_FILE} must hold one full 40-hex FORGE commit SHA, got '${FORGE_PIN}'")
endif()
# A pin bump re-runs configure (and so re-fetches) on the next build.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FORGE_PIN_FILE}")

set(FORGE_SOURCE_DIR "" CACHE PATH "FORGE checkout (empty: fetch the commit in cmake/FORGE_PIN)")
find_package(Git QUIET)
function(_prism_forge_git out_var)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -c advice.detachedHead=false ${ARGN}
                  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
  set(${out_var} "${_out}" PARENT_SCOPE)
  set(${out_var}_RC "${_rc}" PARENT_SCOPE)
  set(${out_var}_ERR "${_err}" PARENT_SCOPE)
endfunction()

if(NOT FORGE_SOURCE_DIR)
  # Fetch exactly the pinned commit, shallow (git fetch --depth 1 <url> <sha>,
  # the same content-addressed fetch third_party/MANIFEST.toml pins use).
  # FetchContent's GIT_SHALLOW only works for branch or tag names, and a full
  # clone of FORGE is ~120 MB, so this does the fetch itself. The fetched path
  # is a normal variable, never written to the FORGE_SOURCE_DIR cache entry,
  # so a pin bump is always honoured.
  if(NOT GIT_FOUND)
    message(FATAL_ERROR "PRISM requires FORGE at ${FORGE_PIN}: git not found to fetch it. "
                        "Install git or set -DFORGE_SOURCE_DIR=/path/to/FORGE.")
  endif()
  set(_forge_src "${CMAKE_BINARY_DIR}/_deps/forge-src")
  set(_forge_head "")
  if(EXISTS "${_forge_src}/.git")
    _prism_forge_git(_forge_head -C "${_forge_src}" rev-parse HEAD)
  endif()
  if(NOT _forge_head STREQUAL FORGE_PIN)
    message(STATUS "PRISM: fetching FORGE ${FORGE_PIN} from ${FORGE_GIT_URL}")
    file(REMOVE_RECURSE "${_forge_src}")
    file(MAKE_DIRECTORY "${_forge_src}")
    _prism_forge_git(_o -C "${_forge_src}" init -q)
    if(_o_RC EQUAL 0)
      _prism_forge_git(_o -C "${_forge_src}" fetch -q --depth 1 "${FORGE_GIT_URL}" "${FORGE_PIN}")
    endif()
    if(_o_RC EQUAL 0)
      _prism_forge_git(_o -C "${_forge_src}" checkout -q --detach FETCH_HEAD)
    endif()
    if(NOT _o_RC EQUAL 0)
      message(FATAL_ERROR "PRISM: fetching FORGE ${FORGE_PIN} failed: ${_o_ERR}\n"
                          "Set -DFORGE_SOURCE_DIR=/path/to/FORGE for an offline build.")
    endif()
    _prism_forge_git(_forge_head -C "${_forge_src}" rev-parse HEAD)
    if(NOT _forge_head STREQUAL FORGE_PIN)
      message(FATAL_ERROR "PRISM: fetched FORGE is at '${_forge_head}', not the pin ${FORGE_PIN}")
    endif()
  endif()
  set(FORGE_SOURCE_DIR "${_forge_src}")
  set(FORGE_COMMIT "${FORGE_PIN}")
else()
  # A local checkout (CI checks one out at the pin). Say which commit it is;
  # a commit other than the pin is a warning, never silent.
  set(FORGE_COMMIT "unknown (not a git checkout)")
  if(GIT_FOUND AND EXISTS "${FORGE_SOURCE_DIR}/.git")
    _prism_forge_git(_forge_head -C "${FORGE_SOURCE_DIR}" rev-parse HEAD)
    if(_forge_head_RC EQUAL 0)
      set(FORGE_COMMIT "${_forge_head}")
    endif()
  endif()
  if(NOT FORGE_COMMIT STREQUAL FORGE_PIN)
    message(WARNING "PRISM: FORGE_SOURCE_DIR=${FORGE_SOURCE_DIR} is at ${FORGE_COMMIT}, "
                    "not the pinned ${FORGE_PIN} (cmake/FORGE_PIN). The build is not the reviewed one.")
  endif()
endif()
foreach(_req core/include/forge/core/log.hpp os/include/forge/os/os.hpp inference/include/forge/inference/inference.hpp)
  if(NOT EXISTS "${FORGE_SOURCE_DIR}/${_req}")
    message(FATAL_ERROR "PRISM requires FORGE (odin-loki/FORGE ${FORGE_PIN}); '${_req}' not found under "
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
  foreach(_t forge_core forge_os forge_inference forge_verify forge_simd_mv)
    if(NOT TARGET ${_t})
      message(FATAL_ERROR "FORGE at ${FORGE_SOURCE_DIR} has no target ${_t} (FORGE pin ${FORGE_PIN} changed the targets?)")
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
  # prism_forge_inference: the AI engine (FORGE inference + verify + simd MV kernels).
  add_library(prism_forge_inference INTERFACE)
  target_link_libraries(prism_forge_inference INTERFACE prism_forge forge_inference forge_verify forge_simd_mv)
endif()
message(STATUS "PRISM: FORGE (mandatory) ${FORGE_COMMIT} from ${FORGE_SOURCE_DIR}")
