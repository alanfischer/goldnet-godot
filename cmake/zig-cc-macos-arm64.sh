#!/bin/sh
exec zig cc -target aarch64-macos.11.0 "$@"
