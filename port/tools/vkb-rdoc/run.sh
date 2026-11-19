#!/bin/bash
# usage: run.sh script.py (env RDOC_CAP, RDOC_OUT)
RDOC_LIBS=${RDOC_LIBS:-/home/runner/tools/libs}; RD=${RDOC_DIR:-/home/runner/tools/renderdoc}
export LD_LIBRARY_PATH=$RDOC_LIBS/usr/lib/x86_64-linux-gnu:$RDOC_LIBS/usr/lib/x86_64-linux-gnu/libproxy:$LD_LIBRARY_PATH
export DISPLAY=${DISPLAY:-:97}; export DISPLAY
timeout ${T:-900} $RD/bin/qrenderdoc --ui-python $1 2>&1 | tail -20
