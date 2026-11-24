# 地平线 4 DLSS Movie＋XeSS 帧生成

[English](README.md) · [下载 Release](https://github.com/widmonstertony/forza-horizon-4-dlss5-xefg/releases)

适用于 Steam 版《极限竞速：地平线 4》的图形插件。在 D3D12 场景通道执行两遍 Movie 神经渲染和 DLSS 超分，再用 XeSS 在两张真实帧之间补一帧。HDR 仪表和小地图取自游戏当前帧，插帧后单独合成。

## 实测

Steam **1.478.564.0**、RTX 4090 Laptop GPU、**3840×2400 HDR**、120 Hz、超高预设、2× MSAA。基准赛行驶期间取 78.6 秒，XeSS 统计提交 2,760 张真实帧和 2,759 张生成帧，平均 **70.3 FPS**。游戏成绩页的真实渲染帧率为 **35 FPS**。抽查了加速、换挡和夜间路段，未看到速度数字和转速指针重影。[测试记录](docs/VALIDATION.md)

## 安装

1. 从 Release 下载并解压安装包，关闭游戏。
2. 自行准备许可允许使用的 `nvngx_dlssnr.dll` 和 `nvngx_dlss.dll`，校验 SHA-256 与 `runtime-manifest.json` 一致。安装包提供插件、ReShade、桥接模块和未经修改的 Intel XeSS-FG、XeLL 运行库。NVIDIA 的两个运行库需单独准备。
3. 在解压目录打开 PowerShell，运行：

   ```powershell
   .\Install.ps1 -GamePath "D:\SteamLibrary\steamapps\common\ForzaHorizon4" -RuntimeSource "D:\GraphicsRuntimes"
   ```

   安装器会验证游戏版本和文件校验值。如遇同名图形插件或配置，且内容不同，会在复制前停止。
4. 从 Steam 正常启动，使用 **3840×2400 HDR**。按 `Insert` 打开 ReShade。单独关闭插帧可将 `FH4Dlss5\frame-generation.ini` 中的 `Enabled` 改成 `0`，改回 `1` 即可重新开启。平时使用请保持 `PreviewGenerated=0`。

卸载时先退出游戏，只移除 `runtime-manifest.json` 中由本次安装添加的文件。请保留安装前已经存在的文件。

## 原理

插件按着色器匹配 Steam 版的场景解析和色调映射通道。FH4 先按输出分辨率绘制场景，插件再取 1920×1200 的场景执行两遍 Movie，由 DLSS 超分回 3840×2400，并与原生场景融合，以减轻暗部彩噪、保留车身细节。

XeSS 帧生成使用游戏的深度、运动矢量和绘制仪表之前的 HDR 场景。插件从完整游戏帧中提取原生 UI 像素，插帧后再将其贴回。 [Intel XeSS-FG 开发指南](https://www.intel.com/content/www/us/en/developer/articles/technical/xess-fg-developer-guide.html)

当前结果来自上述单一硬件和游戏版本。微软商店版和其他分辨率尚未验证。 [源码构建说明](src/README.md) · [第三方许可](THIRD_PARTY_NOTICES.md)
