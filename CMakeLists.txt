cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME waddle)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

set(EXTENSION_SOURCES
    src/waddle_extension.cpp
    src/recycling_adapter.cpp
    src/recycling_key.cpp
    src/recycling_cache.cpp
    src/adapter/recycling_state.cpp
    src/adapter/recycling_settings.cpp
    src/adapter/client_state.cpp
    src/adapter/operators.cpp
    src/adapter/plan_keys.cpp
    src/adapter/pipeline_breakers.cpp
    src/adapter/plan_rewrites.cpp
    src/adapter/blocks.cpp
    src/adapter/block_ranges.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Local experiment only: configure with -DWADDLE_ABLATION=AGGREGATE,
# DISTINCT, JOIN_BUILD or CTE to measure one breaker kind's contribution.
if(WADDLE_ABLATION)
  if(NOT WADDLE_ABLATION MATCHES "^(AGGREGATE|DISTINCT|JOIN_BUILD|CTE)$")
    message(FATAL_ERROR "Unknown WADDLE_ABLATION: ${WADDLE_ABLATION}")
  endif()
  target_compile_definitions(${EXTENSION_NAME} PRIVATE WADDLE_ABLATE_${WADDLE_ABLATION})
  target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE WADDLE_ABLATE_${WADDLE_ABLATION})
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
