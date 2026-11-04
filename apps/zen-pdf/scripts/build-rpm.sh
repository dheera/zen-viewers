#!/usr/bin/env bash
set -eu
root=$(cd -- "$(dirname -- "$0")/../../.." && pwd)
exec "$root/scripts/build-rpm.sh"
