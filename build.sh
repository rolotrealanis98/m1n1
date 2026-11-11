#!/bin/bash
# u1 build: the fastbuild command (same container, pinned librust.a, same make line as the storage-line
# build.sh, source mounted at /fb/src). Usage: S1N_STAGE=3 ./build.sh   (default stage 3)
# For a hardware binary use release-check (two clean builds of HEAD) and snapshot, see BUILD-NOTES.md.
set -euo pipefail
FB=/Users/rolo/Work/agents/win-el2/tooling/fastbuild-20261110
exec "$FB/fb" build unified --stage "${S1N_STAGE:-3}"
