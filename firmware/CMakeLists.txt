# StackChan VolC Open — ESP-IDF top-level CMake
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.4.1")
add_definitions(-DFIRMWARE_VERSION=\"${PROJECT_VER}\")
add_compile_options(-Wno-missing-field-initializers)

# Pull upstream components from ../upstream/<name> into EXTRA_COMPONENT_DIRS.
# Users run scripts/fetch_repos.sh once before the first build.
set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
endif()

set(STACKCHAN_UPSTREAM_COMPONENTS
    "${PROJECT_ROOT}/upstream/volc_conv_ai/volc_conv_ai"
)

foreach(component_dir ${STACKCHAN_UPSTREAM_COMPONENTS})
    if(EXISTS "${component_dir}/CMakeLists.txt")
        list(APPEND EXTRA_COMPONENT_DIRS "${component_dir}")
    endif()
endforeach()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(stackchan-volc-open)
