#!/bin/sh
exec zig c++ -target aarch64-linux-gnu.2.28 "$@"
