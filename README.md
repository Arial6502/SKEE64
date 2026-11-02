# SKEE64 (personal edit)

A personal edit of SKEE64, the SKSE plugin behind RaceMenu (`skee64.dll`), based on [expired6978/SKSE64Plugins](https://github.com/expired6978/SKSE64Plugins).

## License

The original license applies. Upstream has no license, so all rights stay with the original authors and this edit may not be redistributed. No builds are published here, and please don't upload any.

## Branches

The upstream code targeted Skyrim 1.7.99 on the original (non-NG) SKSE. Each branch ports it back to one older runtime.

| Branch | Skyrim | SKSE |
|---|---|---|
| `1.6.1170` | 1.6.1170 | 2.2.8 |
| `1.5.97` | 1.5.97 | 2.0.20 |

On 1.5.97, every hook was moved to its 1.5.97 address and checked against the executable, with assembly stubs rewritten where the function prologues differ.

## Changes

### Integrated fixes

- [OverlayFix](https://github.com/naitro2010/OverlayFix): overlay null checks, overlay ordering, guarded `DeepCopy` calls and the morph request collapsing. SKSE's `AddTask` queue can also run on a worker thread, so all scene work now goes through a queue drained after `Main::Update`.
- [SKEEcosaveLoadCrashFix](https://github.com/Acro748/SKEEcosaveLoadCrashFix): a cosave string missing from the string table no longer crashes the load. The bad entry is skipped and counted in the log.
- [SkeeHeadPartLookupFix](https://github.com/servaltullius/SkeeHeadPartLookupFix): head part lookups by name use a table built at data load instead of a full scan.

### Body morphs

The morph code is rewritten to be faster. All shapes of an actor are morphed in one parallel batch, with normals and tangents from a new SSE/AVX2 kernel picked at runtime. Skin partitions are reused instead of copied on every update, and armor covering several biped slots is morphed once instead of once per slot.

Bench results for the compute stage, using a body and a heavy armor with 40 active morphs per shape on a Ryzen 9 5950X:

| Compute stage | Before | After | Gain |
|---|---|---|---|
| Body, 3 shapes, 25k vertices | 6.0 ms | 0.76 ms | 7.9x |
| Body + armor, 13 shapes, 522k vertices | 35.0 ms | 7.1 ms | 4.9x |
| Normals and tangents only, body | 6.3 ms | 0.57 ms | 11x |
| Same, normals off (delta apply, 40 morphs, body) | 0.26 ms | 0.19 ms | 1.3x |
| Delta apply, all morphs, 13-shape armor | 23.2 ms | 10.5 ms | 2.2x |
| TRI delta memory, 13-shape armor | 387 MB | 97 MB | 4x |

The "before" column leaves out the skin partition copy and GPU buffers the old code made on every update, so real gains in game are larger.

Shapes that get attached again with their morphs already applied are now rebuilt from their stored original. Before, races like MuErinRace doubled body morphs after loading a save.

### Containers and JSON

- Containers moved to abseil (`flat_hash_map`, `node_hash_map`, `btree_map` and the matching sets).
- JSON parsing moved from jsoncpp to glaze. A repair pass keeps old presets loading, including malformed numbers like `3.5.0` and legacy code page text. Parsing all test presets went from 125 ms to 52 ms.

### Other

- Compatibility with [my fork](https://github.com/Arial6502/ArmorUnlimited) of naitro2010's ArmorUnlimited, so morphs and overlays reach armor on its extra biped slots.
- The public interfaces other plugins call keep their vtable layout and `std` container types, so existing plugins keep working.

## Settings

New keys in `skee64.ini`:

| Section | Key | Default | Effect |
|---|---|---|---|
| `Overlays/Data` | `bLegacyOverlayOrder` | 1 | The last overlay draws on top, as it used to |
| `Overlays/Data` | `bHideUnusedOverlays` | 1 | Hides overlays that are transparent or use the default or blank texture |
| `Features` | `bMainThreadTransforms` | 1 | Runs node transform updates on the main thread |
| `Features` | `bTransformCollisionFix` | 1 | Updates world data without collision before transforms |
| `General` | `bLogMorphTiming` | 0 | Logs the timing of each morph batch |

## Building

The project builds with CMake, Ninja and vcpkg. SKSE is fetched from source at the release each branch targets. You need:

- Visual Studio with the C++ desktop workload
- CMake and Ninja
- vcpkg, with `VCPKG_ROOT` set

```bat
cmake --preset config-release
cmake --build --preset build-release
```

`_CompileDLL.bat` does the same from a plain command prompt. There are also `avx`, `avx2` and `avx512` presets. If `ENV_PLUGIN_COPY_DIR` is set, the DLL and PDB are copied there after each build. Configure with `-DPL_DEPLOY_TO_FOLDER=OFF` to turn that off.

## Disclaimer

This edit is almost entirely vibecoded. Most of the code was written with an AI assistant and has only been partly tested in game. Use it at your own risk, and don't report its bugs to the original RaceMenu author.
