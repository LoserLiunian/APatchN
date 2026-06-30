# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What `apdc/` is

A **C/C++23 port of the Rust `apd` daemon** (`../apd/src/`). Same binary, same on-device
behavior, same multi-call `argv[0]` dispatch — just C++ instead of Rust. Each `cpp/<x>.cpp`
is a 1:1 mirror of `../apd/src/<x>.rs` (e.g. `event.cpp`↔`event.rs`, `module.cpp`↔`module.rs`,
`supercall.cpp`↔`supercall.rs`). Output is a standalone arm64 ELF meant to replace
`app/libs/arm64-v8a/libapd.so`.

**For all daemon *behavior*** — boot lifecycle/stage order, A/B module install, su flags,
root reconciliation guards (the `uid 0 & 2000` exception, the `"su"` superkey gotcha in the
uid-listener), safe mode, file layout under `/data/adb/ap/` — the root
[`../CLAUDE.md`](../CLAUDE.md) "`apd` daemon" section is authoritative. This port mirrors it;
don't re-document it here. This file covers only what's *different about the C++ port*.

> **Status: not wired into Gradle.** The Gradle build still compiles the Rust crate
> (`cargoBuild`→`buildApd`→`libapd.so`). `apdc/` is built **standalone** via `./build.sh`.
> The `CMakeLists.txt` header comment ("Gradle drives it through an Exec task") describes the
> *intended* end state, not the current wiring. Don't assume `./gradlew assemble` builds this.

## Build

```sh
./build.sh                 # Release, output: build-cmake/apd
./build.sh Debug
ANDROID_NDK_HOME=/path/to/ndk NINJA=/path/to/ninja ./build.sh Release
cmake --build build-cmake  # incremental rebuild after the first configure
```

- CMake **3.28+** (C++ modules / `CXXModules`), Ninja, NDK toolchain, `ANDROID_PLATFORM=android-26`,
  `arm64-v8a` only. Defaults: NDK at `D:/Android/Sdk/ndk/29.0.14206865`, `ninja` on `PATH`.
- Release flags mirror the Rust profile intent: `-O2`, `--gc-sections`, stripped (`-s`),
  `-fno-stack-protector -fomit-frame-pointer`, hidden visibility.
- **Exceptions + RTTI are ON** (unlike the `apjni` JNI lib) — deliberately, so the error model below works.
- `.gitignore` ignores `build-cmake/` and the generated `cpp/version.hpp`. No generated files are committed.

To deploy a local build: copy `build-cmake/apd` → `app/libs/arm64-v8a/libapd.so`.

## Error model — anyhow mapped onto C++ exceptions (`cpp/result.hpp`)

Subsystem functions **throw** `apd::Error` (a `std::runtime_error`); `main.cpp` catches
`std::exception` at the top, logs `Error: <what>`, returns 1. This is the C++ image of Rust's
`anyhow::Result` + `?` + `.context()`:

| Rust | C++ (`result.hpp`) |
|---|---|
| `bail!(msg)` | `bail(msg)` / `bailf("{}…", …)` |
| `ensure!(cond, msg)` | `ensure(cond, msg)` / `ensuref(cond, "{}…", …)` |
| `.context("…")?` | `rethrow_with_context(e, "…")` |

There is no `Result<T>` return type — success returns normally, failure throws. Don't add
error-code returns; follow the throw/catch pattern the rest of `cpp/` uses.

## CMake-time codegen (mirrors Rust `include_str!` / `build.rs`)

Two headers are generated into `build-cmake/generated/` at configure time — they don't exist
on a clean checkout, so unresolved `version.hpp` / `embedded_scripts.hpp` before a build is expected:

- **`version.hpp`** (from `cpp/version.hpp.in`): `APD_VERSION_CODE` / `APD_VERSION_NAME`.
  Uses the **same `10000 + 200 + git-rev-list-count` offset** as everywhere else — this is now a
  **4th copy** of that formula (root `build.gradle.kts`, `apd/build.rs`, `.github/workflows/build.yml`,
  **and `apdc/CMakeLists.txt`**); edit them together. `VERSION_NAME` = `git describe --tags --always`
  (leading `v` stripped). Needs a full clone for a correct count.
- **`embedded_scripts.hpp`**: embeds `scripts/installer.sh` and `scripts/banner` as raw-string
  `INSTALLER_SH` / `BANNER` constants (the `include_str!` equivalent). **Edit `scripts/installer.sh`**
  — it's the live one. `CMAKE_CONFIGURE_DEPENDS` re-runs CMake when either changes.

## Vendored `third_party/` (all built with `-w`)

Static libs the daemon links; the upstream Rust crate got these from Cargo/system, here they're vendored:

- **miniz** — zip extraction (module installs).
- **Lua 5.4** — module scripting. **Compiled as C++** (`-x c++`, sources retargeted to `LANGUAGE CXX`)
  so Lua errors propagate as **C++ exceptions, not `longjmp`** — this is what lets `lua_CFunction`s
  throw/catch across Lua frames safely (the `mlua` model the Rust side relied on). `lua.c`/`luac.c`
  (interpreter/compiler mains) are excluded.
- **libsepol** — SELinux policy engine. **Monolithic-policy path only**: `module_to_cil.c` is excluded
  (the lone CIL dependency); the whole `cil/` subtree is unused by the build.
- **magiskpolicy** — Magisk v23 SELinux glue on libsepol; the CLI `magiskpolicy.cpp` main is excluded
  (apd provides its own entry via `sepolicy.cpp`).
- **systemproperties** (bionic) + **nanopb** + **resetprop** (Magisk v23) — back the `resetprop` multi-call.

## Keep-in-sync (in addition to the root CLAUDE.md's list)

- **Supercall ABI** is now re-declared in a **4th place**: `app/.../uapi/scdefs.h` ⇆ `apd/src/supercall.rs`
  ⇆ external KernelPatch ⇆ **`apdc/cpp/supercall.{hpp,cpp}`**. Command numbers, `0x1158` magic, and the
  `su_profile` layout must agree. (Like the Rust side, apd's supercall version stays the independent
  `0/11/1` — not the KernelPatch `version` file. Don't "fix" the drift.)
- **Module-config wire format** (`APTM` magic, LE, key≤256B/value≤1MB/≤32 entries): `cpp/module_config.cpp`
  ⇆ `../apd/src/module_config.rs` ⇆ the host spec/regression test below.
- **`MODULE_UPDATE_DIR`** (`defs.hpp`) ⇆ `module_installer.sh` — same coupling as the Rust port.

## Tests

`test/test_config_format.py` — the **only** test: an executable spec + regression guard for the `APTM`
module-config binary format, runnable on the host with no deps/framework:

```sh
python test/test_config_format.py
```

Run it after any change to `module_config.cpp`'s serialization. (Reuse on-device to inspect a real
`persist.config` the daemon writes.) There is no C++ unit-test harness; the daemon is validated by
building + on-device behavior, same as the Rust crate.
