#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="${ROOT_DIR}/binaries"

mkdir -p "${BIN_DIR}"

echo "[build] compiling libmanual.dylib"
clang++ -std=c++17 -dynamiclib "${ROOT_DIR}/manual_dylib.cpp" -o "${BIN_DIR}/libmanual.dylib"

echo "[build] compiling libcomplex_manual.dylib"
clang++ -std=c++17 -dynamiclib "${ROOT_DIR}/complex_manual_dylib.cpp" -o "${BIN_DIR}/libcomplex_manual.dylib"

echo "[build] compiling manual_runner"
clang++ -std=c++17 "${ROOT_DIR}/manual_runner.cpp" "${ROOT_DIR}/manual_mapper.cpp" -o "${BIN_DIR}/manual_runner"

echo "[build] artifacts available in ${BIN_DIR}"
