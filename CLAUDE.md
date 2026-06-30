# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

**APatch** is the *manager app* for a kernel-based Android root solution (arm64 only, kernels 3.18–6.12). It does **not** contain the kernel patch itself — the patch core (`kpimg`) and injector (`kptools`) live in the external [KernelPatch](https://github.com/bmax121/KernelPatch) ("KP") project and are **downloaded at build time** (pinned by `kernelPatchVersion` in the root `build.gradle.kts`). UI and APM (Magisk-like) module support are derived from KernelSU. License GPL-3; package `me.bmax.apatch`.

The repo is three codebases that all talk to a patched kernel through one ABI (the **supercall**):
- **`app/`** — Kotlin/Jetpack-Compose manager + a small C++ JNI bridge (`apjni` → `libapjni.so`).
- **`apd/`** — Rust userspace daemon, a multi-call binary bundled as `libapd.so`, installed to `/data/adb/apd`.
- **`apdc/`** — an in-progress **C/C++23 port of `apd/`** (CMake/Ninja, built standalone via `apdc/build.sh`, **not yet wired into Gradle** — Gradle still builds the Rust crate). Each `apdc/cpp/<x>.cpp` mirrors `apd/src/<x>.rs`. See [`apdc/CLAUDE.md`](apdc/CLAUDE.md).
- Native blobs and shell scripts shipped in the APK.

Two module systems coexist:
- **APM** — Magisk-style userspace modules managed by `apd` (`/data/adb/modules/`).
- **KPM** — kernel modules injected into the kernel via supercall (inline-hook / syscall-table-hook), managed through the JNI layer.

`settings.gradle.kts` includes **only `:app`**. `apd/` is a Cargo crate, **not** a Gradle subproject — Gradle builds it via `Exec` tasks.

## Build & commands

Everything goes through Gradle; it compiles the Rust daemon and downloads KP assets as `preBuild`/JNI-merge dependencies. There is **no test suite** (no `test`/`androidTest` sources); "verifying a build" means a clean assemble. Lint is non-gating (`lint { abortOnError=false }`).

```sh
./gradlew assembleDebug                       # debug APK (not minified)
./gradlew assembleRelease                      # release APK (R8 + shrink + LTO)
./gradlew clean assembleDebug assembleRelease  # exact CI command
./gradlew printVersion                         # dump git-derived versionCode/Name
./gradlew :app:cargoBuild                      # build apd via cargo-ndk
./gradlew clean                                # ALSO runs apdClean -> cargoClean (wipes libapd.so + Rust target)
cd apd && cargo ndk -t arm64-v8a build --release   # build the daemon alone
```

APK output: `app/build/outputs/apk/<type>/`, named `APatch_<versionCode>_<versionName>_<branch>`. The release APK **doubles as a recovery-flashable zip** (see Boot-image patching).

### Toolchain (exact pins, all in root `build.gradle.kts` + `gradle/libs.versions.toml`)
- Gradle wrapper 9.3.1, AGP 9.0.0, Kotlin 2.3.10, KSP 2.3.5 (KSP must track Kotlin), JDK **21** (CI uses the *jetbrains* distribution).
- NDK `29.0.14206865`, build-tools `36.1.0`, compile/target SDK 36, minSdk 26, CMake 3.28.0+.
- `kernelPatchVersion = "0.13.1"` (root `build.gradle.kts`) → surfaced as `BuildConfig.buildKPV`; drives the KP download URLs. Must point at a real KernelPatch GitHub release.
- Rust: `cargo-ndk` + `rustup target add aarch64-linux-android` are required for **any** `assemble` (the `buildApd` task is wired into `merge{Debug,Release}JniLibFolders`). Crate is **edition 2024**, release profile `panic = "abort"` (a Rust panic aborts the daemon), `lto = "fat"`, `codegen-units = 1`, `strip = true`.
- `ccache` is auto-detected on `PATH` and used for native builds.

### Gradle task graph
- `preBuild` → `downloadKpimg` (→ `app/src/main/assets/kpimg`), `downloadKptools` (→ `app/libs/arm64-v8a/libkptools.so`), `downloadCompatKpatch` (→ `libkpatch.so`, **hardcoded to KP `0.10.7`**, a legacy compat shim — *not* tied to `kernelPatchVersion`; don't "fix" it to the variable), `mergeScripts`.
- `mergeScripts` copies `scripts/update_binary.sh`→`update-binary` and `scripts/update_script.sh`→`updater-script` into `app/src/main/resources/META-INF/com/google/android/` (the flashable-zip installer; that `resources/` dir is git-ignored). `update_script.sh` is an empty placeholder.
- `cargoBuild` (`cargo ndk -t arm64-v8a build --release` in `apd/`) → `buildApd` copies `apd/target/aarch64-linux-android/release/apd` → `app/libs/arm64-v8a/libapd.so`.

### Version derivation (3 copies — keep in sync)
`versionCode = 1*10000 + (git rev-list --count HEAD) + 200`. The same offset is duplicated in `build.gradle.kts`, `apd/build.rs`, and CI `.github/workflows/build.yml` — edit all three together. **Requires a full clone** (CI uses `fetch-depth: 0`); a shallow clone yields a wrong versionCode and breaks tagging.
- APK `versionName` = `git rev-parse --verify --short HEAD` (short commit hash). Note: the Gradle helper is misleadingly named `getGitDescribe()` but does **not** run `git describe`.
- `apd`'s `VERSION_NAME` *does* use `git describe --tags --always` (leading `v` stripped), generated by `apd/build.rs` into `$OUT_DIR` and `include_str!`'d in `defs.rs`. `build.rs` only reruns on `../.git/HEAD` and `../.git/refs/` changes.

### Signing
The `apksign` plugin reads Gradle/env props `KEYSTORE_FILE`/`KEYSTORE_PASSWORD`/`KEY_ALIAS`/`KEY_PASSWORD`. CI does **not** use these — it signs post-build with `kevin-david/zipalign-sign-android-release` and secrets `SIGNING_KEY`/`ALIAS`/`KEY_STORE_PASSWORD`/`KEY_PASSWORD`.

### What's in git vs generated
- **Committed prebuilts (no regen task — do not delete):** `app/libs/arm64-v8a/libbusybox.so`, `libbootctl.so`.
- **Downloaded, git-ignored:** `assets/kpimg`, `libkptools.so`, `libkpatch.so` (+ `*.kpm`).
- **Built locally, git-ignored:** `libapjni.so` (CMake project `apjni`), `libapd.so` (cargo).
- Native build philosophy: C++23/C23, `-DANDROID_STL=none` + prefab `cxx::cxx` (the `libcxx` catalog version **must equal** `androidCompileNdkVersion`), LTO + LLVM **Polly**, release `-Ofast -flto=full`. The `apjni` `.so` is stripped of build-id/ident notes post-build.
- CI mutates `gradle.properties` at runtime (appends `parallel`/`vfs.watch`/`jvmargs`/verbose native output, and defensively `sed`-strips `org.gradle.configuration-cache=true` — currently a **no-op**, that flag isn't in the committed file). Dependabot updates gradle, github-actions, and **cargo (`apd/`)** daily.

## The supercall ABI — the cross-cutting contract

The patched kernel overlays a syscall on **`__NR_truncate` (nr 45)**. Userspace authenticates with the **superkey** and issues commands by number. This ABI is **independently re-declared (copy-pasted, not shared via a header) in three places that must all agree**, or the kernel silently mis-dispatches:
- `app/src/main/cpp/uapi/scdefs.h` — C command numbers, magic, structs.
- `apd/src/supercall.rs` — Rust mirror.
- the external **KernelPatch** project.

Call shape: `syscall(45, key /*const char* */, ver_and_cmd(cmd), arg1..arg4)`. Packing: `ver_and_cmd = (version_code<<32) | (0x1158<<16) | (cmd & 0xFFFF)`, where `version_code = (MAJOR<<16)|(MINOR<<8)|PATCH`. KP validates the `0x1158` magic, reads the 16-bit cmd, and version-gates (`>= 0x0a05`).
- **Deliberate version drift, do not "fix":** the C side (`app/src/main/cpp/version`) reads `0.13.0`; `apd/src/supercall.rs` hardcodes `0/11/1`. Both clear the `>=0.10.5` gate, so they're intentionally independent.
- `SUPERCALL_HELLO_MAGIC = 0x11581158` (echo string `"hello1158"`). `SUPERCALL_KEY_MAX_LEN = 0x40`, `SUPERCALL_SCONTEXT_LEN = 0x60`. `struct su_profile { uid_t uid; uid_t to_uid; char scontext[0x60]; }` (Rust mirror uses `i32`/`i32` to match `uid_t` width).
- Command groups (`scdefs.h`): hello/log/ver `0x1000–0x1009`; superkey `0x100a–0x100c`; su `0x1010/0x1011`; KPM `0x1020–0x1022`,`0x1030–0x1032`; kstorage `0x1040–0x1045` (groups: su-list=0, exclude-list=1); su-admin grant/revoke/nums/list/profile `0x1100–0x1104`, get/set sctx `0x1105/0x1106`, get/reset path `0x1110/0x1111`, get-safemode `0x1112`. kstorage WRITE/READ pack `((offset<<32)|dlen)` into one arg.
- The kernel stores only a **hash** of the superkey: `h = 1000000007; for c in key: h = h*31 + c` (`scdefs.h:hash_key`). Passing the literal string `"su"` works **only if the caller's uid is already on the su-allow list** (the kernel substitutes the real key) — this is why the manager and parts of `apd` use `"su"` as a sentinel.

### JNI surface (`app/src/main/cpp/apjni.cpp` ↔ `Natives.kt`)
- `Natives` does `System.loadLibrary("apjni")`; methods are **registered dynamically in `JNI_OnLoad` via a `gMethods[]` table** (NOT `Java_`-name-mangled). The string name + JNI signature in `gMethods` must exactly match each `external fun native*` in `Natives.kt` — a mismatch is an `UnsatisfiedLinkError` at runtime, not compile time.
- Every wrapper passes `APApplication.superKey` as the first arg. A null superkey jstring → `abort()` (SIGABRT, by design). Most methods are `@FastNative`; the three blocking ones (`nativeLoad/Unload/ControlKernelPatchModule`) deliberately are not.
- `nativeSuProfile` / `nativeControlKernelPatchModule` do hardcoded `FindClass`+`GetFieldID` into `Natives$Profile` (`uid`/`toUid`/`scontext`) and `Natives$KPMCtlRes` (`rc`/`outMsg`) — renaming those `@Keep` inner classes/fields breaks the native silently at runtime.
- Native helpers in `cpp/`: `jni_helper.hpp` (vendored lsplant — `JUTFString`, `ScopedLocalRef`, `JNI_SafeInvoke`, `JNI_RegisterNatives`). Prefer these over raw JNI.

## The `apd` daemon (`apd/src/`)

A single multi-call binary at `/data/adb/apd`. `main.rs` = `cli::run()`.

### argv[0] dispatch (runs BEFORE clap, `cli.rs`)
The kernel exec's this binary under different names; order matters:
1. ends with `kp`/`su` → `apd::root_shell()` (the `su` implementation).
2. ends with `resetprop` → `resetprop::resetprop_main` (Magisk-compatible prop tool).
3. ends with `magiskpolicy` → `sepolicy::policy_main` (SELinux policy patcher).
4. else → clap subcommands (kebab-cased): `post-fs-data`, `services`, `boot-completed`, `uid-listener`, `module …`, `resetprop`, `sepolicy`. Global `-s/--superkey`.

The `resetprop`/`magiskpolicy` symlinks → `/data/adb/apd` are (re)created every post-fs-data by `assets::ensure_binaries()`; `busybox`/`kptools` in `bin/` are real copies. (`apd.rs`'s `su` supports `-c`, `-l`, `-p`, `-s SHELL`, `-M/--mount-master`, `--no-pty`, `-v/-V`; switches to the global mount ns if `-M` or `/data/adb/.global_namespace_enable` is `1`.)

### Module subcommands
`module {install,uninstall,undo-uninstall,enable,disable,action,lua,list,config}` — all first call `utils::switch_mnt_ns(1)` to enter init's (global) mount namespace.
- **A/B-style install:** install extracts into `MODULE_UPDATE_DIR` (`/data/adb/modules_update/`, **do not rename — coupled to `module_installer.sh`**), then just `touch`es `/data/adb/ap/update`. The module is **not active until reboot**, when `handle_updated_modules()` promotes staging → `MODULE_DIR` (`/data/adb/modules/`) preserving `disable`/`remove` flags. `install_module` requires `sys.boot_completed==1`.
- Flag files per module: `disable`, `remove` (deferred to next boot's `prune_modules`), `update`, `skip_mount`.
- `module config` target = `internal.<name>` (with `--internal`) else env `AP_MODULE` (set for module scripts). Store: binary files `persist.config`/`tmp.config` under `/data/adb/ap/module_configs/<id>/` (magic `APTM`, LE; key≤256B, value≤1MB, ≤32 entries; key regex `^[a-zA-Z][a-zA-Z0-9._-]+$`). `merge_configs` = persist then temp overlay; `tmp.config` is wiped early each boot.
- **Lua modules:** `lua.rs` uses `Lua::unsafe_new()`; `<id>.lua` per module; stage functions are invoked **with the superkey as the argument** (it's intentionally exposed to module lua).
- **Metamodule** (`metamodule.rs`): at most one, tracked by symlink `/data/adb/metamodule` → its module dir. An enabled metamodule with `metainstall.sh` **hijacks all regular installs**, and can block installs while unstable.

### Boot lifecycle (`event.rs`) — KP invokes `apd -s <key> <stage>` at each stage
`report_kernel(event,state)` is the notify channel: it execs the `truncate` command with argv `[key|"su", "event", <event>, <state>]` (resolved via `PATH` to `/system/bin/truncate`, the supercall entry — not a real truncate).

`on_post_data_fs` order: `report "before"` → load `su_path` into kernel → inject magisk sepolicy to `/sys/fs/selinux/load` → `privilege_apd_profile` (puts apd's own PID on the su-allow list, `to_uid=0`, scontext `u:r:magisk:s0`) → `clear_all_temp_configs` → **if `has_magisk()`: warn, report "after", RETURN** (Magisk coexistence: apd yields all module work) → create/rotate `/data/adb/ap/log/` (0700) and spawn time-limited `logcat` (45s → `logcat.log`) + `dmesg -w` (120s → `dmesg.log`) capturers → safe-mode check → promote `modules_update` → `prune_modules` → `restorecon` → apply per-module `sepolicy.rule` → metamodule `metamount.sh` → module `post-fs-data.sh`/lua → `load_system_prop` → `run_stage("post-mount")` → report "after".
- `run_stage` order each stage: metamodule `<stage>.sh` → global `/data/adb/<stage>.d/*` → per-module `<stage>.sh` → lua `<stage>`. Used for `service` (non-blocking), `boot-completed`.
- `boot-completed` also spawns `apd uid-listener`: inotify on `/data/system/packages.list.tmp` (1s debounce) → `refresh_ap_package_list` → reconcile root grants. **Gotcha:** the uid-listener hardcodes the superkey to literal `"su"` (not the real key), unlike the rest of `apd`.
- **Root reconciliation** (`refresh_ap_package_list`): revokes su for every uid **except 0 and 2000** (hardcoded — don't drop this guard or you brick root/adb), reconciles `package_config` against `/data/system/packages.list`, then `SU_GRANT_UID` where `allow==1 && exclude==0`, `KSTORAGE_WRITE` (exclude group 1) where `allow==0 && exclude==1`.
- **Safe mode** (`persist.sys.safemode`/`ro.sys.safemode==1`, or `SU_GET_SAFEMODE`): disables all modules (only effective during boot; `disable_all_modules` is a no-op once `sys.boot_completed==1`).

### apd dead code (do not wire up)
- `apd/src/installer_bind.sh` — divergent copy of `installer.sh`, **zero references**. Edit `installer.sh` (the live one, `include_str!`'d by `module.rs`) for install behavior.
- `cli.rs` defines a local `enum Sepolicy { Check }` that is unused (the live `Sepolicy` subcommand uses `crate::sepolicy::Args`).

### Forked deps
`apd/Cargo.toml` pulls **Cargo git forks from github.com** (`AndroidPatch/{zip-extensions-rs,java-properties,loopdev,ap_policy}`, `Kernel-SU/ksu_props`) — these are unrelated to the Gradle `jitpack.io` repo (which is only for `libsu` on the Android side). `mlua` is `lua54,vendored`; `rustix` `all-apis`.

## On-device filesystem layout

Canonical constants: `apd/src/defs.rs`, mirrored in `APatchApp.kt` companion and `cpp/uapi/scdefs.h`.

| Path | What |
|---|---|
| `/data/adb/apd` | the daemon (= `libapd.so`); also backs `su`/`resetprop`/`magiskpolicy` via argv[0] |
| `/data/adb/ap/` | working dir |
| `/data/adb/ap/bin/` | `apd`,`magiskpolicy`,`resetprop` (symlinks→`/data/adb/apd`); `busybox`,`kptools` (real copies) |
| `/data/adb/ap/log/` | boot logs (0700), rotated `*.old.log` each boot |
| `/data/adb/ap/su_path` | kernel su exec path. **Seeded to `/system/bin/su` (LEGACY) on AP install**; `/system/bin/kp` (DEFAULT) is the KP-only default that uninstall resets back to |
| `/data/adb/ap/package_config` | CSV root-grant DB `pkg,exclude,allow,uid,to_uid,sctx` |
| `/data/adb/ap/module_configs/` | per-module `persist.config`/`tmp.config` |
| `/data/adb/ap/kpms/` | staged KPM `.kpm` files (Kotlin side) |
| `/data/adb/ap/.aprc` | rc sourced as `$ENV` for root shells |
| `/data/adb/modules/` , `/data/adb/modules_update/` | active modules / staged updates (A/B) |
| `/data/adb/metamodule/` | symlink → the one active metamodule |
| `/data/adb/{post-fs-data,service,boot-completed}.d/` | global stage scripts |
| `/data/adb/.global_namespace_enable` | `"1"` → all `su` shells use global mount ns |
| `/system/bin/truncate` | the supercall entry (`SUPERCMD`, syscall 45) |
| `/dev/.safemode` , `/dev/.need_reboot` | flags (`.need_reboot` set by `markNeedReboot()`, read at app start) |
| `/data/system/packages.list[.tmp]` | watched by uid-listener; uid reconciliation source of truth |

SELinux: apd runs as `u:r:magisk:s0`; granted apps default to `u:r:untrusted_app:s0` (`DEFAULT_SCONTEXT`). `scdefs.h` also defines `u:r:kp:s0` / `u:r:kernel:s0`.

## Kotlin app (`app/src/main/java/me/bmax/apatch/`)

### App bootstrap & state machine (`APatchApp.kt`)
- `onCreate`: **exits** (Toast + 5s sleep + `exitProcess`) if `arm64-v8a` not in `SUPPORTED_ABIS`; in non-DEBUG builds `verifyAppSignature("1x2twMoHvfWUODv7KkRRNKBzOfEqJwRKGzJpgaz18xk=")` and **self-uninstalls in an infinite `ACTION_DELETE` loop on mismatch** (renaming the package or re-signing breaks release builds). Then sets `superKey = "su"` (hardcoded; see the standing TODOs about superkey theft) and builds a shared OkHttp client.
- `APApplication` is its own `Thread.UncaughtExceptionHandler`: any uncaught exception routes to `CrashHandleActivity` (extras `exception_message`/`thread`) and `exitProcess(10)` — **you won't see the normal system crash dialog** during debugging.
- **The whole state machine is driven by the `superKey` setter:** it calls `Natives.nativeReady`, seeds `kpStateLiveData`/`apStateLiveData` (the two `State` enums everything observes), then spawns a thread that `su(0,null)`s and computes update/reboot states. KP "needs update" compares **build time, not version** (`Version.getKpImg()` of the bundled `kpimg` vs `Natives.kernelPatchBuildTime()`), using `!=` to deliberately allow downgrades.
- `installApatch()`/`uninstallApatch()` place/remove the on-device files (copy `libapd.so`→`/data/adb/apd`, symlink `magiskpolicy`/`resetprop`→apd, copy busybox/kptools, run `libmagiskpolicy.so --magisk --live`, then `APatchCli.refresh()`). **Uninstall leaves `/data/adb/ap/`, `package_config`, `su_path`, `kpms/` behind** — full wipe only happens in `PatchesViewModel.doUnpatch` (`rm -rf /data/adb/ap/`).

### Root shell (`util/APatchCli.kt`)
- `APatchCli.SHELL` (main) + `GLOBAL_MNT_SHELL`; `getRootShell(globalMnt)` returns one. Build **fallback chain**: `truncate <superKey> -Z u:r:magisk:s0` → `libkpatch.so <superKey> su -Z magisk [--mount-master]` → `su [-mm]` → `sh`. The kernel intercepts the `truncate <superkey>` invocation to hand back a root shell.
- `APatchCli.refresh()` **reflects into libsu's `MainShell`** private statics (`isInitMain`/`mainShell`/`mainBuilder`) — a libsu bump or R8 rule change can break this in **release only**.
- Module ops shell out to the daemon: `execApd(args)` → `"/data/adb/apd <args>"`. `hasMagisk()` here runs `nsenter --mount=/proc/1/ns/mnt which magisk` (enters PID 1's mount ns) — **distinct** from apd's `which::which("magisk")`; the two can disagree.

### UI (Compose) — `ui/`
- Navigation = **raamcosta Compose Destinations** + KSP. Each screen is a `@Composable @Destination<RootGraph>` (start = `HomeScreen`). KSP generates `com.ramcosta.composedestinations.generated.{NavGraphs, *Destination}` into `build/generated/ksp/<variant>/kotlin` — **these symbols don't exist on a clean checkout until a build runs** (unresolved references then are expected, not a bug). Destination name = `<FunName>Destination` (e.g. `fun Patches` → `PatchesDestination`, not `…ScreenDestination`); renaming a screen renames its destination and breaks every `navigate()` site. `ksp { arg("compose-destinations.defaultTransitions","none") }` disables built-in transitions; `MainActivity` supplies custom ones keyed off the bottom-bar route set.
- `BottomBarDestination` enum (5 tabs: Home, KModule/KPM, SuperUser, AModule/APM, Settings) with `kPatchRequired`/`aPatchRequired` flags; `MainActivity` gates visibility from `apStateLiveData` and renders a `NavigationBar` (portrait) or `NavigationRail` (landscape).
- Screens: `Home` (status dashboard, install/reboot entry), `InstallModeSelect`→`Patches` (boot patching), `Install` (shared APM/KPM module-install log view; defines `enum MODULE_TYPE { KPM, APM }`), `APM`/`KPM` (module managers), `SuperUser` (per-app root grants), `Settings`, `About`, `ExecuteAPMAction`.
- Theme (`theme/Theme.kt`): dynamic Monet color (Android 12+) or one of **19 custom palettes**; a module-level `refreshTheme` LiveData applies changes live (no Activity recreate). **Adding a palette = a new `*Theme.kt` + a dark `when` branch + a light `when` branch in `Theme.kt` + a `colorsList()` entry in `Settings.kt` + the string resource — all in sync.**
- ViewModels (`ui/viewmodel/`): `PatchesViewModel` (boot patching), `APModuleViewModel`/`KPModuleViewModel`, `SuperUserViewModel` (its `apps` is a process-wide companion reused by WebUI), `KPModel` (its `ExtraType.desc`/`TriggerEvent.event` strings are passed verbatim to `kptools` and must match KP).

### WebUI, services, self-update
- `WebUIActivity` serves a module's `/data/adb/modules/<id>/webroot` via `WebViewAssetLoader` domain `mui.kernelsu.org`, JS bridge named `ksu` (`WebViewInterface`: `exec`/`spawn`/`toast`/`fullScreen`/`enableInsets`/`listPackages`/…; each `exec` builds its own root shell). `internal/colors.css` is empty until the Compose `MonetColorsProvider.UpdateCss()` has run.
- `services/` + `aidl/IAPRootService.aidl`: a libsu `RootService` exposing `getPackages()` (uses `rikka.parcelablelist.ParcelableListSlice` to dodge `TransactionTooLarge`), enumerating all user profiles; consumed by `SuperUserViewModel`.
- Self-update: a cached `OkHttpClient` (User-Agent `APatch/<versionCode>`) + `util/Downloader.kt`/`LatestVersionInfo.kt` hit the GitHub releases API and download an APK (About screen). `AndroidManifest` allows cleartext (`network_security_config`).
- `util/Version.kt` getters (`installedApdV*`) **mutate static caches as a side effect** — call the function, don't read the field cold.

## Boot-image patching (two independent paths, no shared code)

1. **In-app** (`PatchesViewModel.kt`): `prepare()` builds `<filesDir.parent>/patch/`, symlinking native libs to bare names (`libkptools.so`→`kptools`, `libbusybox.so`→`busybox`, `libkpatch.so`→`kpatch`, `libbootctl.so`→`bootctl`) and copying assets (`boot_patch.sh`, `boot_unpatch.sh`, `boot_extract.sh`, `util_functions.sh`, `kpimg`). Scripts run as `./busybox sh <script>` with `ASH_STANDALONE=1`.
   - `boot_extract.sh [next]` discovers the boot partition (echoes `SLOT=`/`BOOTIMAGE=`).
   - `boot_patch.sh <superkey> <bootimage> [flash] [extra kptools args]` → `kptools unpack` → aborts with **`exit 0`** (success code!) if `CONFIG_KALLSYMS=y` missing → backs up `ori.img` → `kptools -p … -k kpimg -o kernel` (adds `-S <key>` unless key is the sentinel `"su"`) → `repack` → `flash_image` if flashing to a block device. Trailing `-M/-A/-V/-T/-E` flags embed KPM modules.
   - `doUnpatch` is **restore-from-backup**: copies `/data/adb/ap/ori.img`→`new-boot.img`, then `boot_unpatch.sh` just flashes it; then wipes `/data/adb/ap/`.
   - `INSTALL_TO_NEXT_SLOT` uses the committed `libbootctl.so` (`bootctl get-current-slot`/`set-active-boot-slot`) and writes a `/data/adb/post-fs-data.d/post_ota.sh` marker that runs `bootctl mark-boot-successful`. `PATCH_ONLY` writes the patched image to Downloads.
   - Manager superkey validation when a real key is set: length 8–63, ≥1 digit, ≥1 letter.
2. **Recovery flashable zip = the APK itself**: `META-INF/com/google/android/update-binary` (from `scripts/update_binary.sh`) picks `assets/UninstallAP.sh` if the zip name contains `uninstall`, else `assets/InstallAP.sh`. These call `lib/arm64-v8a/libkptools.so` directly + raw `dd` to `/dev/block/by-name/boot$slot`, with **no superkey**, and don't use `util_functions.sh`.

> `util_functions.sh` hardcodes a stale `APATCH_VER='0.10.4'`/`164` unrelated to the real version (which comes from apd / `Version`). Most of that file is Magisk-derived recovery machinery unused by the in-app path.

## Conventions & gotchas

- **arm64-v8a only** — build is ABI-locked and the app exits at startup on other ABIs. Never add another ABI.
- **Translations go through [Weblate](https://hosted.weblate.org/engage/APatch), not PRs.** Only edit `app/src/main/res/values/strings.xml` (the source locale); the ~43 `values-*/` dirs are Weblate-owned. `localeConfig` is auto-generated (`generateLocaleConfig = true`) — don't add a manual one. (Per-language FAQs live in `docs/`; full docs are in the separate `AndroidPatch/APatchDocs` repo.)
- New text files are **LF** (`.gitattributes` `* text eol=lf`); only `*.bat`/`*.cmd` are CRLF; `*.so`/`*.apk`/`*.png`/etc. are declared binary.
- Release builds are **R8-minified + resource-shrunk + multidex** with an aggressive `proguard-rules.pro` (`-repackageclasses`, `-allowaccessmodification`, `-overloadaggressively`; the only explicit `-keep` is `org.ini4j.spi.**`). JNI/reflection types survive only via `@Keep` (`Natives`, `Natives.Profile`, `Natives.KPMCtlRes`). R8-sensitive bugs (e.g. the libsu reflection in `APatchCli.refresh()`) are **release-only** — debug isn't minified.
- Required kernel configs (README): `CONFIG_KALLSYMS=y` + `CONFIG_KALLSYMS_ALL=y` (full); `KALLSYMS_ALL=n` is "initial support" only.

### "Keep these in sync" (the highest-value constraints)
- **Supercall ABI** (command numbers, `0x1158` magic, `su_profile`/`SuProfile` layout): `cpp/uapi/scdefs.h` ⇆ `apd/src/supercall.rs` ⇆ external KernelPatch.
- **JNI table**: `cpp/apjni.cpp` `gMethods[]` names+signatures ⇆ `Natives.kt` `external fun native*` (and the reflected `Natives$Profile`/`$KPMCtlRes` fields).
- **versionCode offset** (`+10000+200`): `build.gradle.kts` ⇆ `apd/build.rs` ⇆ `.github/workflows/build.yml`.
- **`libcxx` version** ⇆ `androidCompileNdkVersion`.
- **Color palette** additions: theme file + 2 `when` branches in `Theme.kt` + `colorsList()` in `Settings.kt` + string resource.
- **`MODULE_UPDATE_DIR`** name ⇆ `module_installer.sh`.
- Don't reach for `installer_bind.sh` or `cli.rs`'s local `enum Sepolicy` — both are dead.
