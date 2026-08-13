FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq \
      build-essential python3-pip strace git ca-certificates cmake \
    && rm -rf /var/lib/apt/lists/*

# Real NVIDIA headers (headers only; no driver, no GPU required to compile)
RUN pip3 -q install --no-cache-dir nvidia-cuda-runtime-cu12 nvidia-nvml-dev-cu12

ENV NV_INC=/usr/local/lib/python3.10/dist-packages/nvidia
WORKDIR /work
