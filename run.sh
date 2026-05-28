#!/bin/bash
# ═══════════════════════════════════════════════════════════════
#  CyberVision C2 - Quick Launcher
#  Builds (if needed) and runs the C2 console
# ═══════════════════════════════════════════════════════════════

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Build if binary doesn't exist
if [ ! -f "./cybervision" ] || [ ! -d "./tools/classes" ]; then
    echo "  [*] First run - building..."
    bash build.sh
fi

# Run
exec ./cybervision "$@"
