#!/usr/bin/env bash
set -euo pipefail

sudo apt-get install -y \
  pkg-config \
  libavformat-dev \
  libavcodec-dev \
  libavutil-dev \
  libswscale-dev \
  ffmpeg
