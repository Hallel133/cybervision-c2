#!/bin/bash
# ═══════════════════════════════════════════════════════════════
#  CyberVision C2 - Build Script
#  Compiles: Java tools (protocol engine) + C++ console (CLI)
# ═══════════════════════════════════════════════════════════════

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo ""
echo "  ⚡ CyberVision C2 Build System"
echo "  ═══════════════════════════════"
echo ""

# ─── Check dependencies ──────────────────────────────────────────────────────
check_dep() {
    if ! command -v "$1" &>/dev/null; then
        echo "  [!] Missing: $1"
        echo "      Install with: $2"
        exit 1
    fi
}

check_dep "javac" "apt install default-jdk / brew install openjdk"
check_dep "g++"   "apt install g++ / xcode-select --install"

echo "  [✓] Java: $(java -version 2>&1 | head -1)"
echo "  [✓] G++:  $(g++ --version | head -1)"
echo ""

# ─── Step 1: Compile Java tools ──────────────────────────────────────────────
echo "  [1/3] Compiling Java tools..."
mkdir -p tools/classes
find tools/src -name "*.java" | xargs javac -d tools/classes -source 11 -target 11 2>&1
echo "  [✓] Java tools compiled → tools/classes/"

# ─── Step 2: Compile C++ console ──────────────────────────────────────────────
echo "  [2/3] Compiling C++ console..."
g++ -std=c++17 -pthread -O2 -I console/include -o cybervision console/src/main.cpp 2>&1
echo "  [✓] C++ console compiled → ./cybervision"

# ─── Step 3: Create directories ──────────────────────────────────────────────
echo "  [3/3] Setting up directories..."
mkdir -p logs loot web
echo "  [✓] Directories ready (logs/, loot/, web/)"

# ─── Done ─────────────────────────────────────────────────────────────────────
echo ""
echo "  ═══════════════════════════════════════════"
echo "  ✓ Build complete!"
echo ""
echo "  Run:  ./cybervision"
echo "  Or:   ./cybervision -p 8888 -w 9090"
echo ""
echo "  Android APK:"
echo "    cd android && ./gradlew assembleDebug"
echo "    APK: android/app/build/outputs/apk/debug/"
echo "  ═══════════════════════════════════════════"
echo ""
