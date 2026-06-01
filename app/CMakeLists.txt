cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hello_pico_w)
target_sources(app PRIVATE src/main.c)

if(DEFINED ENV{HOST_WORKSPACE})
    zephyr_compile_options(
        -fdebug-prefix-map=/workspace=$ENV{HOST_WORKSPACE}/workspace
        -fdebug-prefix-map=/workspace/app=$ENV{HOST_WORKSPACE}/app
    )
endif()
