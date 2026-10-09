#!/usr/bin/env bash
set -euo pipefail
git ls-files '*.cpp' '*.h' | xargs clang-format --dry-run --Werror
cmake --build --preset conan-debug
ctest --preset conan-debug --output-on-failure --no-tests=error
