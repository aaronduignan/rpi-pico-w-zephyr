FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV ZEPHYR_SDK_VERSION=1.0.1
ENV ZEPHYR_VERSION=v4.4.0
ENV ZEPHYR_BASE=/workspace/zephyr

# System deps
RUN apt-get update && apt-get install -y --no-install-recommends \
    git cmake ninja-build gperf ccache dfu-util device-tree-compiler wget \
    python3-dev python3-pip python3-setuptools python3-tk python3-wheel \
    xz-utils file make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1 \
    && rm -rf /var/lib/apt/lists/*

# Zephyr SDK — minimal base + ARM toolchain + host tools
RUN BASE=https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZEPHYR_SDK_VERSION} \
    && wget -q ${BASE}/zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64_minimal.tar.xz -O /tmp/sdk.tar.xz \
    && tar -xf /tmp/sdk.tar.xz -C /opt && rm /tmp/sdk.tar.xz \
    && wget -q ${BASE}/toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz -O /tmp/toolchain.tar.xz \
    && tar -xf /tmp/toolchain.tar.xz -C /opt/zephyr-sdk-${ZEPHYR_SDK_VERSION} && rm /tmp/toolchain.tar.xz \
    && wget -q ${BASE}/hosttools_linux-x86_64.tar.xz -O /tmp/hosttools.tar.xz \
    && tar -xf /tmp/hosttools.tar.xz -C /opt/zephyr-sdk-${ZEPHYR_SDK_VERSION} && rm /tmp/hosttools.tar.xz \
    && /opt/zephyr-sdk-${ZEPHYR_SDK_VERSION}/setup.sh -c

ENV ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-${ZEPHYR_SDK_VERSION}

# Python deps
RUN pip3 install west \
    && wget -q https://raw.githubusercontent.com/zephyrproject-rtos/zephyr/${ZEPHYR_VERSION}/scripts/requirements-base.txt \
        -O /tmp/requirements-base.txt \
    && pip3 install -r /tmp/requirements-base.txt \
    && rm /tmp/requirements-base.txt

WORKDIR /workspace
