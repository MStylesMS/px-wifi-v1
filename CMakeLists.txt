cmake_minimum_required(VERSION 3.16)

# Shared components live in the sibling 'components' repo.
# Override with LOKI_COMPONENTS_DIR env var for non-standard layouts.
if(DEFINED ENV{LOKI_COMPONENTS_DIR})
    set(LOKI_BASE "$ENV{LOKI_COMPONENTS_DIR}")
else()
    set(LOKI_BASE "${CMAKE_CURRENT_LIST_DIR}/../components")
endif()

set(EXTRA_COMPONENT_DIRS "${LOKI_BASE}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(loki-wp1)
