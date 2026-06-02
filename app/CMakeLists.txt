cmake_minimum_required(VERSION 3.20.0)

# Add custom DTS binding search path for our CYW43 BT HCI shared-bus binding
list(APPEND DTS_EXTRA_CPPFLAGS -I${CMAKE_CURRENT_SOURCE_DIR}/dts)
# Register our out-of-tree BT HCI driver as a Zephyr module
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/drivers/bt)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hello_pico_w)
target_sources(app PRIVATE src/main.c)

if(DEFINED ENV{HOST_WORKSPACE})
    zephyr_compile_options(
        -fdebug-prefix-map=/workspace=$ENV{HOST_WORKSPACE}/workspace
        -fdebug-prefix-map=/workspace/app=$ENV{HOST_WORKSPACE}/app
    )
endif()
