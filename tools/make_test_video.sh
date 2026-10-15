#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The 1080p50 test video used for the frame and cursor latency tests
# (FFmpeg's testsrc2 colour pattern with moving noise on top, so every frame
# changes everywhere): 30 s, H.264.
#
#   tools/make_test_video.sh [output, default test1080p50.mp4]
#
# Play it fullscreen on the USB display, e.g. mpv --fs --loop test1080p50.mp4.
set -eu
exec ffmpeg -y -loglevel error -f lavfi \
	-i "testsrc2=size=1920x1080:rate=50,noise=alls=12:allf=t" \
	-t 30 -c:v libx264 -preset medium -crf 23 -pix_fmt yuv420p -g 50 \
	"${1:-test1080p50.mp4}"
