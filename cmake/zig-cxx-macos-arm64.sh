#!/bin/sh
exec zig c++ -target aarch64-macos.11.0 "$@"
