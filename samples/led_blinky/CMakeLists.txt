cmake_minimum_required(VERSION 3.20.0)

# WHD requires firmware paths even for GPIO-only use — the blob embedding
# code runs unconditionally when CONFIG_AIROC_WIFI_CUSTOM=y.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Neither firmware produces reliable GPIO behaviour on WL_GPIO0.
# FMAC firmware (43439A0.bin) is theoretically correct — WL_GPIO0 is not
# claimed by the firmware — but cyw43_gpio writes still land erratically.
# BT firmware (wb43439A0_7_95_49_00_bluetooth.bin) is kept below for
# reference; it contends for WL_GPIO0 as a coexistence signal and is worse.
set(FW_IMAGE_NAME    /workspace/modules/hal/infineon/zephyr/blobs/img/whd/resources/firmware/COMPONENT_43439/43439A0.bin)
# set(FW_IMAGE_NAME  ${REPO_ROOT}/firmware/wb43439A0_7_95_49_00_bluetooth.bin)
set(NVRAM_IMAGE_NAME ${REPO_ROOT}/firmware/picow_nvram.txt)
set(CLM_IMAGE_NAME /workspace/modules/hal/infineon/zephyr/blobs/img/whd/resources/clm/COMPONENT_43439/COMPONENT_MURATA-1YN/43439A0.clm_blob)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(led_blinky)
target_sources(app PRIVATE src/main.c)

if(DEFINED ENV{HOST_WORKSPACE})
    zephyr_compile_options(
        -fdebug-prefix-map=/workspace=$ENV{HOST_WORKSPACE}/workspace
        -fdebug-prefix-map=/workspace/app=$ENV{HOST_WORKSPACE}
    )
endif()
