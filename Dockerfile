FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV ZEPHYR_SDK_VERSION=0.16.8
ENV ZEPHYR_VERSION=v3.7.0
ENV ZEPHYR_BASE=/workspace/zephyr

# System deps
RUN apt-get update && apt-get install -y --no-install-recommends \
    git cmake ninja-build gperf ccache dfu-util device-tree-compiler wget \
    python3-dev python3-pip python3-setuptools python3-tk python3-wheel \
    xz-utils file make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1 \
    && rm -rf /var/lib/apt/lists/*

# Zephyr SDK (ARM toolchain) — kept as its own layer so it stays cached
RUN wget -q \
    https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZEPHYR_SDK_VERSION}/zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64.tar.xz \
    -O /tmp/zephyr-sdk.tar.xz \
    && tar -xf /tmp/zephyr-sdk.tar.xz -C /opt \
    && /opt/zephyr-sdk-${ZEPHYR_SDK_VERSION}/setup.sh -t arm-zephyr-eabi -h -c \
    && rm /tmp/zephyr-sdk.tar.xz

ENV ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-${ZEPHYR_SDK_VERSION}

# Python deps
RUN pip3 install west \
    && wget -q https://raw.githubusercontent.com/zephyrproject-rtos/zephyr/${ZEPHYR_VERSION}/scripts/requirements-base.txt \
        -O /tmp/requirements-base.txt \
    && pip3 install -r /tmp/requirements-base.txt \
    && rm /tmp/requirements-base.txt

WORKDIR /workspace
