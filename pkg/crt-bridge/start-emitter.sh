#!/bin/sh
cd "$(dirname "$0")" || exit 1
exec ./crt-bridge-emitter --config ./crt-bridge.cfg "$@"
