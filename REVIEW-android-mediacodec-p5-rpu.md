# Review: `feature/android-mediacodec-p5-rpu`

- 审查对象：`fff3ee7a3e feat: 默认传递 Android P5 逐帧 RPU 元数据`（Goodwu，唯一的本方提交）
- 基线：`n7.1.3`（`origin/release/7.1`）
- 改动范围：`libavcodec/mediacodecdec.c`，+431 / -6
- 审查方式：静态阅读 + 与 `hevc/hevcdec.c`、`dovi_rpu*.c`、`mediacodecdec_common.c` 对照。**未编译、未在设备上运行**（本机无 Android NDK 构建环境）。

---

## 1. 方案概述

MediaCodec 只有 `presentationTimeUs` 这一个随 buffer 传递的“用户字段”，Dolby Vision P5 的 RPU（NAL 62）在硬解后丢失。本提交的做法：

1. **输入侧**（`p5_rpu_packet` + `p5_rpu_commit`）：每取到一个 packet，按解码顺序用 `ff_dovi_rpu_parse` 解析 RPU，调用 `ff_dovi_get_metadata` 生成 `AVDOVIMetadata` 快照，连同原始 RPU（`raw_data + 2`）存入 320 项环形表，键为与 `ff_mediacodec_dec_send` 一致的 µs pts。
2. **输出侧**（`p5_rpu_receive` → `p5_rpu_output`）：按输出帧 pts（HW 路径直接取 `MediaCodecBuffer->pts`）查表，唯一匹配时附加 `AV_FRAME_DATA_DOVI_METADATA` 和 `AV_FRAME_DATA_DOVI_RPU_BUFFER`。
3. **flush**：用 epoch 隔离，flush 时清表、`ff_dovi_ctx_flush`，并正确处理 `delay_flush` 的延迟 flush（仅在 `ff_mediacodec_dec_flush()==1` 时清理）。
4. 仅在 `AV_PKT_DATA_DOVI_CONF` 声明 `dv_profile == 5` 时启用，默认附加。

## 2. 与原有框架/架构的符合度

| 方面 | 结论 |
|---|---|
| 解析时机 | ✅ 正确。DoVi RPU 解析是有状态的（`use_prev_vdr_rpu`、`vdr[]` 引用），必须按解码顺序解析；按输出顺序附加。这与 `hevcdec` “解码时 parse、输出时 attach” 的语义一致。每项保存独立的 metadata 快照，避免了直接调用 `ff_dovi_attach_side_data`（它只反映 ctx 的“当前”状态）导致错帧的问题。 |
| side data 格式 | ✅ 与 `hevcdec.c:3660-3663`、`dovi_rpudec.c` 一致：`DOVI_METADATA` 来自 `ff_dovi_get_metadata`，`DOVI_RPU_BUFFER` 为带防竞争字节的 `raw_data + 2`，下游（libplacebo 等）无需区分软硬解。 |
| DOVI_CONF 来源 | ✅ 与 `hevcdec` 相同：init 读 `coded_side_data`，packet 级 side data 更新 `cfg`。 |
| pts 键 | ✅ 选择合理。MediaCodec 没有 per-buffer opaque，pts 是唯一可行的关联键；输入侧复刻了 `ff_mediacodec_dec_send` 的换算（含 `pts && ...` 这一特殊分支），HW 输出直接取 `presentationTimeUs`，避免二次换算误差。 |
| flush / delay_flush | ✅ 考虑周全，epoch 设计正确。 |
| 分层 | ⚠️ 逻辑放在 `mediacodecdec.c` 的 H264/HEVC 包装层，但 pts 的“真值”在 `mediacodecdec_common.c`（`queueInputBuffer` 与 `info->presentationTimeUs`）。输入 pts 换算被**复制**了一份，未来 common 层改动会静默失配。 |
| 可配置性 | ⚠️ 无 AVOption，P5 时强制开启；`attach` 字段恒等于 `enabled`，是死字段。FFmpeg 惯例是用 AVOption（或 `export_side_data`）控制。 |
| 日志 | ⚠️ `MEDIA_KIT_P5_RPU` 是产品私有前缀；flush 与 close 摘要**每次**都以 WARNING 打印，stale eviction 以 ERROR 打印。不符合 FFmpeg 日志等级惯例，也不利于日后 upstream。 |

总体：**核心方案（解码序解析 + pts 关联 + 快照附加）是正确的，也是 MediaCodec 约束下的合理选择**；问题集中在实现冗余、一个匹配逻辑缺陷，以及工程化（配置、日志、分层）。

## 3. 问题（按严重度）

### 3.1 【中】已消费条目仍参与 pts 匹配/重复判定，导致 pts 回绕或重复时元数据被误丢

`p5_rpu_commit` 的重复检测循环和 `p5_rpu_output` 的 `count` 循环都**没有排除 `consumed` 条目**，而消费后条目保持 `valid = 1` 直到 320 帧后被覆盖。

场景：同一 epoch 内 pts 重新出现且未触发 flush —— HLS/concat 的 discontinuity、循环播放未 seek、部分 TS 流 pts 复位等。

- 输入侧：新条目被标为 `duplicate_pts`。
- 输出侧：`count > 1` → 状态 `DUPLICATE_INPUT_PTS`，所有同 pts 条目被清空 → **最多连续 320 帧无 RPU**，P5（IPTPQc2）画面在此期间颜色完全错误。

保留已消费条目本意是检测 `DUPLICATE_OUTPUT_PTS`，但代价过大。建议：
- 重复输入检测只看 `!consumed`；
- 输出匹配优先找 `!consumed` 的唯一项，仅当不存在时才用 consumed 项判断 `DUPLICATE_OUTPUT_PTS`；
- 或者消费后直接 `valid = 0`，放弃该诊断。

同类小问题：`commit` 的重复检测包含即将被覆盖的 `entry` 本身（循环在 `*entry = next` 之前），会把与被驱逐项同 pts 的新条目误判重复。

### 3.2 【中】对整个 AU 做两次切分，其中一次完整拷贝反转义

每个 packet：
1. `p5_rpu_start_code` / `p5_rpu_nal` 手写扫描一遍（只为 `rpu_count`、`rpu_size`、FNV hash —— 仅用于日志）；
2. `ff_h2645_packet_split` 再切分一遍，并对**所有 NAL（含大体积 slice）**做 RBSP 反转义拷贝；`H2645Packet` 为局部变量，每帧 malloc/free。

4K HDR 码率下每帧数百 KB 的额外 memcpy + 扫描，且全部发生在喂数据的关键路径上。建议：
- 只保留一次扫描；DoVi RPU 规范上位于 AU 末尾（`hevcdec` 也只取最后一个 NAL），可从尾部定位 NAL 62；
- 仅对该 NAL 调用 `ff_h2645_extract_rbsp`（或保留一个持久化的 `H2645Packet`/rbsp 缓冲，参照 `hevcdec` 的 `s->pkt`）；
- `rpu_count` / 层 id / tid 校验由同一次扫描给出，删除重复校验。

### 3.3 【低】length-prefixed（hvcC）分支是死代码

`DECLARE_MEDIACODEC_VDEC(hevc, ..., "hevc_mp4toannexb")` 保证进入解码器的 packet 一定是 Annex B，`extradata[21]` 解析 `length_size` 的整段分支与 `H2645_FLAG_IS_NALFF` 永远不会走到。建议删除，减少 ~30 行和维护面。

### 3.4 【低】pts 换算逻辑重复

`p5_rpu_packet` 复制了 `ff_mediacodec_dec_send` 的 “NOPTS→0、`pts != 0` 才 rescale” 规则。建议在 common 层抽出 `ff_mediacodec_pts_to_us()`（或让 send 返回实际入队 pts），两处共用。SW 输出路径用 `frame->pts` 反算 µs，数学上可逆，但同样建议直接使用 `info->presentationTimeUs`（例如在 common 层把它存到 frame 的某处或提供 hook），与 HW 路径统一。

### 3.5 【低】正常情况下的日志噪声与误报

- 每次 seek（flush）一条 WARNING，每次 close 一条 WARNING 摘要 → 应为 `AV_LOG_VERBOSE/DEBUG`。
- MediaCodec 合法地不输出某些帧（seek 到 CRA 后丢弃 RASL、decode-only 帧等），对应条目 320 帧后被驱逐时报 **ERROR** “stale eviction”，属于误报。建议降为 DEBUG，只在摘要里计数。
- `errors <= 20` 的限流在 input/output 共享一个计数器，前面噪声会吃掉后面真正的错误日志。

### 3.6 【低】配置与内存

- 缺少开关：建议加 AVOption（如 `dovi=auto|on|off`，默认 auto=P5 时开启），并去掉恒等的 `attach` 字段。
- `entries[320]`（约 25 KB）+ `DOVIContext` 内联在 `MediaCodecH264DecContext` 中，而该结构也被 **所有** MediaCodec 视频/音频解码器使用（`priv_data_size` 共用）。建议改为 `enabled` 时 `av_calloc`。
- `320` 为魔数，需注释来源（MediaCodec 在途 buffer + DPB 通常远小于此，64~128 足够，且可用 `ff_AMediaCodec` 查询的输入缓冲数推算）。
- `#if defined(__ANDROID__)` 多余（该文件仅在 Android 编译），且让 `enabled` 在非 Android 语法检查时不可达。
- 未对 packet 级 DOVI_CONF 导致的 profile 变更（P5↔其他）做 `enabled` 更新（`hevcdec` 至少会打印提示）。

### 3.7 【低】代码结构

- `p5_rpu.next` + `pending` 作为跨函数暂存状态没有必要：`p5_rpu_packet` 与 `p5_rpu_commit` 总是相邻调用，可合并为一个函数、用局部 entry，减少状态和清理路径。
- 状态用字符串 + `strcmp` 判断，建议改为 enum + 字符串表。
- `MediaCodecH264DecContext` 已被 upstream 更名为 `MediaCodecContext`（`9287fc3bc9`），rebase 到新版本时会冲突；建议把 P5 逻辑独立到 `mediacodec_dovi.c/.h`，mediacodecdec.c 只保留 4 个 hook 调用点，降低后续合并成本。

## 4. 优化建议汇总（推荐的目标形态）

1. **正确性**：修复 3.1（consumed 条目参与匹配）。
2. **性能**：单次扫描、仅反转义 RPU NAL、持久化缓冲（3.2）；删除 hvcC 死分支（3.3）。
3. **架构**：
   - 抽出独立模块（如 `mediacodec_sidedata.c`），提供 `track_input(pkt, pts_us)` / `attach_output(frame, pts_us)` / `flush()` / `uninit()`；
   - pts 换算与输出 `presentationTimeUs` 由 common 层统一提供（3.4）；
   - 该 “pts 关联的逐帧 side data” 机制可泛化到 HDR10+（`AV_FRAME_DATA_DYNAMIC_HDR_PLUS`）等 SEI 元数据，MediaCodec 路径目前同样丢失这些信息。
4. **工程化**：AVOption 开关、日志降级并去掉私有前缀、按需分配内存（3.5、3.6）。
5. **测试**：目前无任何自动化验证。建议至少提供：P5 样片 + B 帧重排、seek（含 `delay_flush=1`）、pts 不连续流三类用例的设备端验证记录；matcher 本身与 MediaCodec 无关，可抽成纯函数写单元测试（`libavcodec/tests/`）。

## 5. 结论

- **方案合理，方向正确**：在 MediaCodec 只能透传 pts 的约束下，“解码序解析 + pts 关联 + 元数据快照”是正确且必要的设计，side data 格式与软解 `hevcdec` 完全一致，flush/epoch 处理周到。
- **需修改后合入**：3.1 是会造成可见画质问题的逻辑缺陷，应优先修复；3.2 的整包双重切分/拷贝建议一并优化。
- 其余为工程化与可维护性改进，若计划长期维护或尝试 upstream，建议完成模块化、AVOption 与日志规范化。
