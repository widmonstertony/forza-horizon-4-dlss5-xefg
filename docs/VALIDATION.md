# Benchmark / 基准测试

- Target / 对象 — Steam 1.478.564.0, RTX 4090 Laptop, 3840×2400 HDR at 120 Hz.
- Settings / 设置 — Ultra, 2× MSAA, VSync off, uncapped, Movie twice and XeSS-FG 2×.
- Measured driving interval / 行驶采样 — 78.561 s, 2,760 real frames + 2,759 generated frames reported by XeSS. Average 70.25 FPS. Across 23 consecutive windows of 120 real frames each, the mean output rates ranged from 67.72 to 74.37 FPS.
- Built-in results / 游戏成绩 — 35 rendered FPS and zero interruptions reported by FH4 on the final run.
- HUD / 仪表 — Checked the speed digits, tachometer needle and minimap in generated-only preview and normal 2× output. The final dark-scene check also covered the section where shadow chroma noise had appeared.

FPS is calculated from XeSS's present status and elapsed time over the sampled driving interval. The window range describes short averages rather than individual-frame lows.

最终帧率按 XeSS 返回的呈现计数和行驶采样时长计算。区间为每 120 张真实帧内的平均输出帧率，并非单帧最低帧率。
