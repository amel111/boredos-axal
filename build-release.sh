#!/bin/bash
# Builds a self-contained axal-installer binary with all kernel files embedded.
# The resulting binary needs NO other files — just drop it in a BoredOS project and run.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GEN_FILE="$SCRIPT_DIR/release_data.h"
OUTPUT="$SCRIPT_DIR/axal-installer-standalone"

# All files to embed (modified + new)
MODIFIED_FILES=(
    "src/core/version.c"
    "src/core/main.c"
    "src/mem/memory_manager.c"
    "src/mem/paging.c"
    "src/arch/syscalls.asm"
    "src/arch/interrupts.asm"
    "src/sys/idt.c"
    "src/sys/lapic.h"
    "src/sys/lapic.c"
    "src/sys/process.h"
    "src/sys/process.c"
    "Makefile"
    "linker.ld"
)

NEW_FILES=(
    "src/mem/pmm.h"
    "src/mem/pmm.c"
    "src/mem/pcpu_cache.h"
    "src/mem/pcpu_cache.c"
    "src/dev/bcache.h"
    "src/dev/bcache.c"
    "src/net/tcp_socket.h"
    "src/net/tcp_socket.c"
)

echo "[*] Generating embedded file data..."

file_to_c_array() {
    local varname="$1"
    local filepath="$2"
    
    if [ ! -f "$filepath" ]; then
        echo "static const unsigned char ${varname}[] = {0};" 
        echo "static const unsigned int ${varname}_len = 0;"
        return
    fi
    
    local size=$(wc -c < "$filepath" | tr -d ' ')
    echo "static const unsigned char ${varname}[] = {"
    od -An -tx1 -v < "$filepath" | sed 's/[[:space:]]\+/ /g' | sed 's/^ //' | sed 's/ *$//' | sed 's/ /,0x/g' | sed 's/^/0x/' | sed 's/$/,/'
    echo "};"
    echo "static const unsigned int ${varname}_len = ${size};"
}

{
    echo "// Auto-generated — DO NOT EDIT"
    echo "#ifndef RELEASE_DATA_H"
    echo "#define RELEASE_DATA_H"
    echo ""
    
    echo "// === AXAL MODIFIED FILES ==="
    idx=0
    for file in "${MODIFIED_FILES[@]}"; do
        file_to_c_array "axal_mod_${idx}" "$SCRIPT_DIR/$file"
        echo ""
        idx=$((idx + 1))
    done
    
    echo "// === AXAL NEW FILES ==="
    idx=0
    for file in "${NEW_FILES[@]}"; do
        file_to_c_array "axal_new_${idx}" "$SCRIPT_DIR/$file"
        echo ""
        idx=$((idx + 1))
    done
    
    echo "static const char *modified_paths[] = {"
    for file in "${MODIFIED_FILES[@]}"; do
        echo "    \"$file\","
    done
    echo "};"
    echo "static const int modified_count = ${#MODIFIED_FILES[@]};"
    echo ""
    
    echo "static const char *new_paths[] = {"
    for file in "${NEW_FILES[@]}"; do
        echo "    \"$file\","
    done
    echo "};"
    echo "static const int new_count = ${#NEW_FILES[@]};"
    echo ""
    
    echo "static const unsigned char *axal_mod_data[] = {"
    for ((i=0; i<${#MODIFIED_FILES[@]}; i++)); do
        echo "    axal_mod_${i},"
    done
    echo "};"
    echo "static const unsigned int axal_mod_sizes[] = {"
    for ((i=0; i<${#MODIFIED_FILES[@]}; i++)); do
        echo "    axal_mod_${i}_len,"
    done
    echo "};"
    echo ""
    
    echo "static const unsigned char *axal_new_data[] = {"
    for ((i=0; i<${#NEW_FILES[@]}; i++)); do
        echo "    axal_new_${i},"
    done
    echo "};"
    echo "static const unsigned int axal_new_sizes[] = {"
    for ((i=0; i<${#NEW_FILES[@]}; i++)); do
        echo "    axal_new_${i}_len,"
    done
    echo "};"
    echo ""
    echo "#endif"
} > "$GEN_FILE"

echo "[✓] Generated $GEN_FILE"
echo "[*] Compiling standalone installer..."

gcc -O2 -static -o "$OUTPUT" "$SCRIPT_DIR/axal-installer-standalone.c" -I"$SCRIPT_DIR"

chmod +x "$OUTPUT"
echo "[✓] Built: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
echo ""
echo "This single binary contains the entire AXAL kernel."
echo "Usage: ./axal-installer-standalone install"
