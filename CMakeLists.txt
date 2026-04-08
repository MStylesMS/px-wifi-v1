cmake_minimum_required(VERSION 3.16)

# Shared components live in the sibling 'px-components' repo.
# Override with PX_COMPONENTS_DIR env var for non-standard layouts.
if(DEFINED ENV{PX_COMPONENTS_DIR})
    set(PX_BASE "$ENV{PX_COMPONENTS_DIR}")
else()
    set(PX_BASE "${CMAKE_CURRENT_LIST_DIR}/../px-components")
endif()

set(EXTRA_COMPONENT_DIRS "${PX_BASE}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(px-wifi-v1)
