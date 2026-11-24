# Forza Horizon 4 DLSS Movie + XeSS Frame Generation

[简体中文](README.zh-CN.md) · [Releases](https://github.com/widmonstertony/forza-horizon-4-dlss5-xefg/releases)

A graphics add-on for the Steam edition of Forza Horizon 4. It runs two Movie neural rendering passes and DLSS Super Resolution inside the D3D12 scene pipeline, then adds one XeSS-generated frame between rendered frames. The original HDR HUD pixels are composited on top of the generated scene.

## What was tested

Steam build **1.478.564.0**, RTX 4090 Laptop GPU, **3840×2400 HDR** at 120 Hz, Ultra and 2× MSAA. Over 78.6 seconds of the in-game benchmark, XeSS reported 2,760 rendered and 2,759 generated presents, averaging **70.3 FPS**. The game's results screen reported **35 rendered FPS**. The speed readout and tachometer needle showed no visible double images in the sampled acceleration, shifting and night scenes. [Benchmark notes](docs/VALIDATION.md)

## Install

1. Download and extract the installation ZIP from Releases. Close the game.
2. Supply the matching `nvngx_dlssnr.dll` and `nvngx_dlss.dll` from your own lawfully obtained runtime. See `runtime-manifest.json` for the tested SHA-256 hashes. The installation ZIP contains the add-on, ReShade, the bridge and Intel's unmodified XeSS-FG and XeLL binaries. NVIDIA's SR and NR binaries must be supplied separately.
3. From PowerShell in the extracted folder, run:

   ```powershell
   .\Install.ps1 -GamePath "D:\SteamLibrary\steamapps\common\ForzaHorizon4" -RuntimeSource "D:\GraphicsRuntimes"
   ```

   The installer verifies the game and dependency hashes. If an existing graphics loader or configuration differs, it stops before copying files.
4. Launch the game from Steam at **3840×2400 HDR**. `Insert` opens ReShade. To switch frame generation off independently, set `Enabled=0` in `FH4Dlss5\frame-generation.ini`. Set it back to `1` to enable it. Keep `PreviewGenerated=0` for normal play.

To remove the add-on, close the game and remove only the mod files listed in `runtime-manifest.json` that you added during installation. Keep files that belonged to your existing setup.

## How it works

The add-on matches the Steam build's known scene resolve and tone-mapping passes. FH4 renders its scene at the output resolution first. The add-on samples that scene at 1920×1200, evaluates Movie twice, runs DLSS SR back to 3840×2400, and blends the result with the native scene to suppress unstable shadows and preserve fine detail.

XeSS-FG receives resolved depth, FH4's velocity and the HDR scene before the HUD. The add-on extracts changed RGBA pixels from the completed frame as the native UI layer. XeSS interpolates the scene and composites that UI layer on top. [Intel XeSS-FG developer guide](https://www.intel.com/content/www/us/en/developer/articles/technical/xess-fg-developer-guide.html)

Compatibility and benchmark numbers refer to the single tested game build and configuration. The Store edition and other resolutions are unverified. See [source build instructions](src/README.md) and [third-party notices](THIRD_PARTY_NOTICES.md).
