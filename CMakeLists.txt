cmake_minimum_required(VERSION 3.15)
project(hybrid_looper C)

include(FetchContent)

# Fetch the official LV2 SDK headers
FetchContent_Declare(
    lv2
    GIT_REPOSITORY https://github.com/lv2/lv2.git
    GIT_TAG        v1.18.10
)
FetchContent_MakeAvailable(lv2)

# Define the shared library target
add_library(my_looper SHARED "my_looper.lv2/my_looper.c")

# Tell CMake to find the headers in both the root and the include folder
target_include_directories(my_looper PRIVATE 
    ${lv2_SOURCE_DIR}
    ${lv2_SOURCE_DIR}/include
)

# Set output name properties
set_target_properties(my_looper PROPERTIES
    PREFIX ""
    OUTPUT_NAME "my_looper"
)
