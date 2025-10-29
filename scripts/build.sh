#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="${ROOT_DIR}/binaries"
SRC_DIR="${ROOT_DIR}/src"
INCLUDE_DIR="${SRC_DIR}/includes"

mkdir -p "${BIN_DIR}"

COMMON_FLAGS=(-std=c++17 -I "${INCLUDE_DIR}")

echo "[build] compiling libmanual.dylib"
clang++ "${COMMON_FLAGS[@]}" -dynamiclib "${SRC_DIR}/manual_dylib.cpp" -o "${BIN_DIR}/libmanual.dylib"

echo "[build] compiling libcomplex_manual.dylib"
clang++ "${COMMON_FLAGS[@]}" -dynamiclib "${SRC_DIR}/complex_manual_dylib.cpp" -o "${BIN_DIR}/libcomplex_manual.dylib"

echo "[build] compiling manual_runner"
clang++ "${COMMON_FLAGS[@]}" "${SRC_DIR}/manual_runner.cpp" "${SRC_DIR}/manual_mapper.cpp" -o "${BIN_DIR}/manual_runner"

echo "[build] artifacts available in ${BIN_DIR}"
