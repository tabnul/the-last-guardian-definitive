#!/bin/bash
# usage: run.sh script.py (env RDOC_CAP, RDOC_OUT)
export LD_LIBRARY_PATH=/home/runner/tools/libs/usr/lib/x86_64-linux-gnu:/home/runner/tools/libs/usr/lib/x86_64-linux-gnu/libproxy:$LD_LIBRARY_PATH
export DISPLAY=${DISPLAY:-:97}; export DISPLAY
timeout ${T:-900} /home/runner/tools/renderdoc/bin/qrenderdoc --ui-python $1 2>&1 | tail -20
