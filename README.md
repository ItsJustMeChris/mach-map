# Manual Mach-O Mapping Prototype

## Overview

This project demonstrates how to load Mach-O dynamic libraries manually—outside of `dyld`—inside the current process. The prototype:

- Parses Mach-O headers (thin or fat) and lays out segments in anonymous memory.
- Applies both legacy dyld rebase/bind opcodes and modern `LC_DYLD_CHAINED_FIXUPS`.
- Resolves imported symbols through a pluggable resolver (defaulting to `dlsym(RTLD_DEFAULT, …)`).
- Exposes exports from the mapped image without ever registering it with `dyld`.

Two sample dylibs are provided—a trivial arithmetic export and a more feature-rich statistics module—along with a runner that drives the loader and verifies the mapped images never appear in the system loader’s image list.

## Layout

- `manual_mapper.cpp/.hpp` – Manual Mach-O loader core.
- `manual_runner.cpp` – Command-line harness that maps one or more dylibs, executes their exports, and prints the process’ dyld image table before/after to prove the manual load is invisible to dyld.
- `manual_dylib.cpp` – Minimal example dylib.
- `complex_manual_dylib.cpp` / `complex_manual_api.hpp` – Larger dylib with global state, chained fixups, and math-library imports.
- `scripts/build.sh` – Helper to build all artifacts into `./binaries`.
- `binaries/` – Output directory for the compiled dylibs and runner (created on demand).

## Building

```bash
./scripts/build.sh
```

The script rebuilds all artifacts with `clang++` and drops them into `./binaries`.

## Running

```bash
# Exercise both example dylibs (default behaviour)
./binaries/manual_runner

# Or target a specific dylib (or multiple)
./binaries/manual_runner ./binaries/libcomplex_manual.dylib
```

- When the simple dylib is loaded, you’ll see the result of `manual_entry(5)` and the exported status string.
- When the complex dylib is loaded, the runner feeds in sample data, prints the computed statistics, histogram, and percentiles, and shows that the dyld image list remains unchanged (`[dyld] image present: no`).

## Custom Symbol Resolution

`manual_mapper` accepts a `LoaderOptions` struct with a `symbolResolver` callback. Supply your own resolver if you want to redirect imports to another image, pre-populate bindings, or stub functions for testing. When omitted, the loader falls back to `dlsym(RTLD_DEFAULT, name)` with automatic leading-underscore stripping.

## Apple Silicon Notes

macOS on arm64 enforces W^X. The loader allocates segments as read/write, performs all fixups, then reapplies per-segment protections, so no `MAP_JIT` or `pthread_jit_write_protect_np` toggles are required. If you extend the prototype to mutate executable pages at runtime, you’ll need to opt into the JIT APIs.
