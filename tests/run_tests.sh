#!/usr/bin/env bash
# シェーダーの共通部を C++ としてコンパイルし、After Effects版と比較します。
#   bash tests/run_tests.sh
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-comment \
    -o "$out/port_regression" "$here/port_regression.cpp"
"$out/port_regression"
