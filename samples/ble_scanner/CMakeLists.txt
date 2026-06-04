cmake_minimum_required(VERSION 3.20.0)

# Shared infrastructure lives at the repo root, two levels above each sample.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

list(APPEND DTS_ROOT ${REPO_ROOT})
list(APPEND ZEPHYR_EXTRA_MODULES ${REPO_ROOT}/drivers/cyw43/bt)
set(EXTRA_DTC_OVERLAY_FILE ${REPO_ROOT}/boards/rpi_pico_rp2040_w.overlay)

set(FW_IMAGE_NAME    ${REPO_ROOT}/firmware/wb43439A0_7_95_49_00_bluetooth.bin)
set(NVRAM_IMAGE_NAME ${REPO_ROOT}/firmware/picow_nvram.txt)
# CLM blob is west-managed; lives in the west workspace regardless of sample.
set(CLM_IMAGE_NAME /workspace/modules/hal/infineon/zephyr/blobs/img/whd/resources/clm/COMPONENT_43439/COMPONENT_MURATA-1YN/43439A0.clm_blob)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ble_scanner)
target_sources(app PRIVATE src/main.c)

if(DEFINED ENV{HOST_WORKSPACE})
    zephyr_compile_options(
        -fdebug-prefix-map=/workspace=$ENV{HOST_WORKSPACE}/workspace
        -fdebug-prefix-map=/workspace/app=$ENV{HOST_WORKSPACE}
    )
endif()
