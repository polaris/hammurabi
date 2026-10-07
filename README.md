# hammurabi
A distributed key-value store based on the Raft consensus algorithm written in modern C++

## Building

Prerequisites: a C++23 compiler (GCC 14 or Apple Clang 17), CMake 3.21 or newer, and Python 3.

Conan, Ninja, clang-format and clang-tidy are installed into a virtual environment from `requirements.txt`, so everyone uses the same versions as CI.

```sh
python3 -m venv .venv && source .venv/bin/activate && pip install -r requirements.txt
conan profile detect --exist-ok
conan install . --build=missing -s build_type=Debug -s compiler.cppstd=23 -c tools.cmake.cmaketoolchain:generator=Ninja
cmake --preset conan-debug
cmake --build --preset conan-debug
ctest --preset conan-debug
```

The first `conan install` may build protobuf and abseil from source, which takes a few minutes. Later runs reuse Conan's package cache.

### Build options

Pass these to `cmake --preset conan-debug`:

| Option | Default | Effect |
|---|---|---|
| `-DHAMMURABI_WARNINGS_AS_ERRORS=ON` | `OFF` | Treat compiler warnings as errors |
| `-DHAMMURABI_SANITIZERS=address,undefined` | empty | Build with the given sanitizers, passed to `-fsanitize=` |
| `-DHAMMURABI_CLANG_TIDY=ON` | `OFF` | Run clang-tidy on hammurabi's own code while compiling |

Check formatting with:

```sh
git ls-files '*.cpp' '*.h' | xargs clang-format --dry-run --Werror
```
