#!/bin/sh
exec zig cc -target x86_64-macos.10.15 "$@"
