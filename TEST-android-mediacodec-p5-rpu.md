# Test Plan: `feature/android-mediacodec-p5-rpu` Android P5 RPU 逐帧元数据传递

- 测试对象:FFmpeg 分支 `feature/android-mediacodec-p5-rpu`,提交 `b4d2ea4ffb`(含整改)及之后版本
- 前一提交 `fff3ee7a3e` 的已知问题(pts 复现时元数据整批丢失、4K 喂数据路径双重拷贝)已在本提交修复,**不要用旧提交出结论**
- 涉及改动:hevc_mediacodec 解码输出帧新增 `AV_FRAME_DATA_DOVI_METADATA` + `AV_FRAME_DATA_DOVI_RPU_BUFFER` side data;新增解码器 AVOption `dovi=auto|on|off`(默认 auto);公共层 `ff_mediacodec_dec_receive()` 签名变化(内部接口);aac/amr/mp3_mediacodec 等音频解码器共用同一上下文结构,需回归

---

## 1. 测试环境准备

**样片**(P0 用例至少需要前 3 个):

| 编号 | 样片 | 要求 |
|---|---|---|
| S1 | DV P5 HEVC(dvhe.05.x / IPTPQc2),含 B 帧 | 1080p 或 4K,TS 或 MP4 |
| S2 | DV P5 高码率流 | ≥ 40 Mbps 4K,用于性能对比 |
| S3 | HDR10(无 RPU)HEVC + 普通 SDR HEVC + H.264 | 回归对照 |
| S4 | pts 异常流:同一 P5 样片 concat 两份(pts 重叠/复位),或 HLS 断续源 | 验证 3.1 修复 |
| S5(可选) | DV P8.1 流 | `dovi=on` 用 |

**设备**:至少高通、联发科各一台,API 24+;有 Amlogic 设备请加入(代码中有其专属 workaround)。

**快速验证手段(不必先做 app 集成)**:

```bash
adb push ffmpeg /data/local/tmp/          # NDK 交叉编译的 CLI,无 surface,走 SW 输出路径
adb shell /data/local/tmp/ffmpeg -v verbose -c:v hevc_mediacodec -i /sdcard/S1.mp4 -f null -
```

**app 内断言**(最终以播放器实际路径为准):对每个输出帧检查
`av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA)` 与
`AV_FRAME_DATA_DOVI_RPU_BUFFER` 应同时存在;可与同一样片软解
(`-c:v hevc`)的逐帧 side data 交叉比对(按 pts 对齐,RPU buffer 字节应一致)。

---

## 2. 日志验收点(`-v verbose`,排查问题时 `-v debug`)

| 日志 | 含义 |
|---|---|
| `Dolby Vision RPU export enabled (profile 5).` | 跟踪已启用(仅 P5+auto 出现一次) |
| `Dolby Vision RPU input problem: ... <status>` | 输入侧异常,限流 ≤20 条 |
| `Dolby Vision RPU output problem: ... <status>` | 输出侧异常,限流 ≤20 条 |
| `Dolby Vision RPU tracking flushed (...)` | 每次 flush/seek 一条(VERBOSE) |
| `Dolby Vision RPU summary: inputs N, outputs N, matched N, input errors N, output errors N, discarded N, unconsumed N, epoch N` | 解码器关闭时一条(VERBOSE) |

**健康标准**:`summary` 中 `inputs == outputs == matched` 且各 error/discarded/unconsumed 为 0(流完整播放、未 seek 截断时)。有 seek 时 `discarded` 允许 >0(被丢弃帧),需可解释。

---

## 3. 用例与通过标准

### P0(必测)

| # | 用例 | 步骤 | 通过标准 |
|---|---|---|---|
| T1 | P5 基线播放(默认 surface 硬解路径) | S1 从头播到尾 | 出现 enabled 日志;全程无 input/output problem WARNING;画面颜色正确(P5 不偏色);summary 满足健康标准 |
| T2 | P5 基线(SW 输出路径) | S1 无 surface 解码(如 CLI `-f null -`) | 同 T1;SW 路径逐帧 side data 完整(3.4 改动后的匹配精度) |
| T3 | 逐帧对应性 | 记录 ≥100 帧 (frame pts → RPU size/hash),与软解同文件结果按 pts 对齐 | 逐帧 RPU 与该 pts 的输入 RPU 一致,无错位 |
| T4 | seek | S1 播放中随机 seek ≥10 次 | seek 后 ≤1s 内 side data 恢复;flush 日志出现;颜色正确;discarded 数值与 seek 次数量级相符 |
| T5 | seek(delay_flush=1) | 设置 `delay_flush=1` 重复 T4 | 同 T4;retain 帧期间无误附着 |
| T6 | 非 P5 回归(auto) | S3 各样片播放 | 无任何 RPU 日志、无 DOVI side data,行为与未改动版本一致 |
| T7 | 音频 mediacodec 回归 | aac / mp3(有则 amr)播放 | 播放正常(公共层 receive 签名改动) |
| T8 | 性能 | S2 整改前(`fff3ee7a3e`)vs 整改后,对比帧率/丢帧/CPU | 不劣化,预期改善(去掉了喂数据路径的全包拷贝) |

### P1(重要)

| # | 用例 | 步骤 | 通过标准 |
|---|---|---|---|
| T9 | pts 复现/回绕(3.1 修复重点) | S4 播放(循环播放不 seek 亦可) | pts 重叠区间各帧**仍逐帧附着 RPU**(旧版会连续丢失);允许少量 DUPLICATE_* WARNING(≤限流 20 条);不得出现大面积 UNMATCHED_OUTPUT_PTS |
| T10 | HLS 断续 / 直播流 | 断续源播放 ≥10min | 无 crash;RPU 逐帧连续;异常 WARNING 可解释 |
| T11 | `dovi=off` | S1 + 显式 off | 零日志、零 side data |
| T12 | `dovi=on` + 非 P5 | S5(P8.1)或 S3 + on | P8.1 有 RPU 时附着;无 RPU 流出现 MISSING_RPU(预期内 WARNING);播放本身不受影响 |
| T13 | 快速 seek 压力 | S1 连续 seek ×100 | 无 crash/ANR/泄漏;summary 数值可解释 |
| T14 | 设备矩阵 | T1/T4/T9 在高通+联发科(+Amlogic)复测 | 同上 |

### P2(加分)

- 长时间播放(≥30min)内存曲线平稳(close 摘要 unconsumed 应为 0)。
- use_ndk_codec=0/1 两条解码路径分别过 T1/T4。
- 后台切换、多实例播放。
- 无 pts 流(NOPTS):出现 MISSING_INPUT_PTS/MISSING_OUTPUT_PTS 且不 crash。

---

## 4. 失败判定与反馈物

**任何一条即判失败**:`ATTACH_ERROR` / 样片正常时的 `PARSE_ERROR` / 非 pts 异常流出现 `DUPLICATE_INPUT_PTS` / T9 场景大面积 UNMATCHED / T6 场景出现任何 RPU 日志 / crash。

**报问题时请附**:`-v verbose` 完整日志(至少含 enabled 与 summary 两条)、样片编码信息(`ffprobe` 的 stream/coded_side_data 段)、设备型号与 Android 版本、复现步骤、所用 `dovi` 取值。
