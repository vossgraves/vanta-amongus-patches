# VANTA patches — Among Us

Morphe patch bundle for Among Us (`com.innersloth.spacemafia`), arm64-v8a.

Two independent patches:

| Patch | Default | What it does |
| --- | --- | --- |
| **Mod menu** | on | Injects `libvanta.so` and a floating overlay; hooks IL2CPP methods at runtime for speed, kill cooldown, kill-anyone, vent-anywhere, cosmetic/account unlocks and task tools. |
| **MicroG RE support** | off | Repoints the bundled Play Games client at microG RE (ReVanced GmsCore) for non-root installs. |

The mod menu patches the game, not the mod. Platinmods-style releases are already-repacked
APKs with a hardcoded menu for one version; this bundle takes the clean APK from APKMirror /
APKPure and produces the same result through Morphe Manager, with the menu rebuilt from
source on every release.

## Install in Morphe

Add this repository to Morphe Manager (**Settings → Patch bundle source → Add**) or paste it
into the bundle URL field:

```
https://raw.githubusercontent.com/vossgraves/vanta-amongus-patches/main/patches-bundle.json
```

Manager reads the index, fetches the signed `.mpp` from the latest release, and verifies it
against the committed public key.

## Targets

| Field | Value |
| --- | --- |
| Package | `com.innersloth.spacemafia` |
| Verified version | `2026.11.3` (versionCode 7616) |
| ABI | arm64-v8a (`config.arm64_v8a` split) |
| Unity | 2022 LTS, IL2CPP, metadata 31 |
| minSdk / targetSdk | 26 / 36 |

Among Us ships as a base APK plus an asset pack plus a per-ABI config split. Patch the
**base APK with the arm64 split loaded** — a base-only APK has no `libil2cpp.so` and the
patch stops with that message. Morphe Manager handles split selection; the CLI takes the
`.apks` / `.xapk`.

## How the mod menu works

```
Morphe patch (Kotlin)                    runtime (arm64)
├── writes lib/arm64-v8a/libvanta.so     ├── JNI_OnLoad, no imports beyond bionic
├── adds SYSTEM_ALERT_WINDOW             ├── dl_iterate_phdr → libil2cpp.so range
├── merges vanta.mpe (Loader, Menu)      ├── 16-byte absolute-branch inline hooks
└── calls Loader.install() in            ├── trampolines in an mmap'd pool
    EosUnityPlayerActivity.onCreate      └── overlay panel drives flag ids over JNI
```

`Loader` retries `Menu.nativeStart(versionCode)` every 1.2 s until the hooks install,
because `libil2cpp.so` is mapped by `libmain.so` a moment after the activity starts. It stops
early when the native side reports an unsupported build instead of spinning for a minute.

Feature wiring:

| Menu switch | Hook | Effect |
| --- | --- | --- |
| Speed | `PlayerControl.FixedUpdate` | writes `MyPhysics.Speed` / `GhostSpeed` for the local player |
| No kill cooldown | `PlayerControl.SetKillTimer` | zeroes the float argument |
| Kill anyone | `PlayerControl.CmdCheckMurder` | calls `RpcMurderPlayer` and skips the validation path |
| Vent anywhere | `Vent.CanUse` (postfix) | forces `canUse` / `couldUse` |
| Unlock cosmetics | `PlayerPurchasesData.GetPurchase` | returns true |
| Unlock features | `EOSManager.IsFreechatAllowed`, `IsFriendsListAllowed`, `IsAllowedOnline`, `IsMinorOrWaiting`, `FullAccount.CanSetCustomName`, `AccountManager.CanPlayOnline` | returns true / clears the minor gate |
| Force task win | `GameManager.CheckTaskCompletion` | returns true |
| Complete my tasks | `PlayerControl.FixedUpdate` | walks `myTasks` and calls `RpcCompleteTask` per id |

The safety rule is version gating: `nativeStart` receives `versionCode`, and `install()`
refuses to hook when it does not equal the table's `kVersionCode`. A stale table degrades
to an inert library — the game runs unmodified instead of crashing on a wrong offset.

## Offsets — after every game update

RVAs live in one file, `native/src/offsets.h`, generated from an IL2CPP dump:

```bash
# 1. dump (Il2CppDumper 6.7.46, .NET 7+, runtime 8 works with roll-forward)
unzip config.arm64_v8a.apk lib/arm64-v8a/libil2cpp.so -d cfg
unzip base.apk assets/bin/Data/Managed/Metadata/global-metadata.dat -d base
DOTNET_ROLL_FORWARD=LatestMajor dotnet Il2CppDumper.dll \
  cfg/lib/arm64-v8a/libil2cpp.so base/assets/bin/Data/Managed/Metadata/global-metadata.dat out

# 2. regenerate the table
python3 tools/dump-offsets.py out/dump.cs \
  --version-code 7616 --version-name 2026.11.3 > native/src/offsets.h

# 3. rebuild the payload
bash native/build.sh
```

`dump-offsets.py` resolves by class + signature, never by line number, and exits non-zero
when a symbol disappears, so a silent zero RVA cannot ship. `native/tests/relocate_test.cpp`
checks the trampoline relocator against the real prologue words of every hooked method — it
is the reason `adrp`-heavy prologues are safe to displace:

```bash
clang++ -std=c++17 -O1 -o /tmp/relocate_test native/tests/relocate_test.cpp && /tmp/relocate_test
clang++ -std=c++17 -O1 -o /tmp/hook_test native/tests/hook_test.cpp -ldl && /tmp/hook_test
```

`native/tests/hook_test.cpp` includes the engine itself and hooks two synthesised functions in
anonymous memory — one whose body fits the displaced window, one that runs past it — then
calls through the detour. It passes on any aarch64 Linux host, which is the target instruction
set. `tools/dexscan.py` does the same
job for dex targets — it is how `EosUnityPlayerActivity.onCreate(Landroid/os/Bundle;)V` and
the Play Services class/method names above were verified without jadx.

## Build

The patch bundle is a normal Morphe repo — Gradle 4.x wrapper included:

```bash
./gradlew buildAndroid        # patches .mpp + extension .mpe
./gradlew generatePatchesList # patches-list.json for third-party tools
```

CI (`.github/workflows/release.yml`) builds, signs and publishes on a tag. Two Gradle
properties are needed for the Morphe registry: `gpr.user` / `gpr.key` (or
`GITHUB_ACTOR` / `GITHUB_TOKEN`).

The native payload builds without the NDK, as long as clang can target aarch64 Android
(Termux clang or `clang --target=aarch64-linux-android26`):

```bash
bash native/build.sh   # → patches/src/main/resources/vanta/libvanta.so
```

Rebuild the `.so` before `./gradlew buildAndroid`; the patch bundle embeds whatever sits in
`patches/src/main/resources/vanta/`.

## Installing on a non-root device

1. Patch the arm64 base APK with Morphe Manager (disable signature verification is not needed —
   Manager re-signs the output).
2. Install microG RE (`app.revanced.android.gms`) if you enabled the MicroG RE patch. AOSP
   devices and devices without Play Services need it; a stock device that already has
   Play Services can skip that patch, but Play Games sign-in will reject the repackaged
   signature.
3. Grant "display over other apps" when the loader asks; the menu bubble appears once the
   game reaches the main menu.

Because the APK is re-signed, cloud saves and Play Games achievements attach to a different
signing identity than your Play Store install. Uninstall the store version or use a second
profile rather than installing over it.

## Limits worth knowing

- **Client-side only.** Every action is a local call that the host validates. Kill-anyone and
  task edits are visible to host-side anticheat mods; use them in private lobbies.
- **arm64 only.** The hook engine writes AArch64 instructions; there is no armv7 path.
- **One version per table.** 2026.11.3 is the verified target. Other versions patch fine and
  produce a working, inert library until the table is regenerated.
- **No overlay on Android < 6.** The panel uses `SYSTEM_ALERT_WINDOW`; the manifest grants
  the permission, the user grants the app op.

## Layout

```
patches/src/main/kotlin/app/vanta/patches/
  amongus/ModMenuPatch.kt        native payload, permission, loader injection
  amongus/MicroGSupportPatch.kt  Play Games → microG RE
  shared/Constants.kt            package, activity, version compatibility
extensions/extension/            Loader (retry + loadLibrary) and Menu (overlay UI)
native/src/vanta.cpp             hook engine, detours, JNI surface
native/src/offsets.h             generated RVA/field table
native/tests/relocate_test.cpp   host-side check of the trampoline relocator
native/tests/hook_test.cpp       host-side end-to-end check of the hook engine
native/build.sh                  clang build, no NDK required
tools/dump-offsets.py            dump.cs → offsets.h
tools/dexscan.py                 minimal DEX reader for target verification
```

## License

GPLv3, inherited from the Morphe patch template. See `LICENSE` and `NOTICE`.
