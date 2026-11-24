# Building the source / 构建源码

Run `build.cmd` from an x64 Visual Studio 2022 developer prompt with the
Windows SDK installed. Pass the five local dependency directories in order.

```bat
build.cmd C:\src\ReShade\include C:\src\ReShade\deps\imgui C:\src\DLSS\include C:\src\XeSS\inc C:\src\minhook
```

- ReShade 6.8.0 and its matching Dear ImGui submodule headers.
- NVIDIA DLSS SDK NGX headers containing `nvsdk_ngx.h` and `nvsdk_ngx_params.h`.
- Intel XeSS SDK 3.0.2 headers.
- MinHook source tree with `include` and `src` directories.

The outputs are `build/fh4-dlss5.addon64` and `build/dlss5_nvngx.dll`. Building
the source does not create the NVIDIA neural models or the Intel runtime.

请从 Visual Studio 2022 的 x64 开发者命令提示符运行上述命令，并按顺序传入
ReShade、其匹配的 ImGui、NVIDIA NGX、Intel XeSS 头文件目录和 MinHook 根目录。
编译产物保存在 `build`，模型和运行库需按根目录说明准备。
