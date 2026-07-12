#!/bin/sh
exec zig c++ -target x86_64-macos.10.15 "$@"
