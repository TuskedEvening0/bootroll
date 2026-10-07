# M11 验收记录（引导器快捷添加）

> 日期：2026-10-07 · 版本线：1.0.0 → 1.1.0 · 依据：ROADMAP §3 / M11_PLAN.md
> 结论：**M11_PLAN §0 验收清单通过**（真机 UEFI 写入验证待用户实测，见 §4）。核心纯逻辑 doctest 全绿，gcc 0 错 0 警，CI 矩阵全绿，Xvfb 冒烟存活。

## 1. 逐项验收结果

| # | 验收项 | 结果 | 证据 |
|---|---|---|---|
| 1 | 核心纯逻辑 + doctest | ✅ | `tests/test_loader_presets.cpp` 11 用例/55 断言：三加载器主路径命中、BOOTX64+limine.conf/cfg 确认 Limine、BOOTX64 单独存在=systemd 兜底（verify 备注，ROADMAP "备选"语义）、limine 证据 > systemd 兜底、主路径命中时抑制兜底归属、多加载器共存每 preset 一条、空/异构 ESP 无命中、探测路径恒为反斜杠前缀形态。全套件 **75 用例/593 断言全过**（基线 64/538） |
| 2 | UI 快捷添加 | ✅（实现与自动化冒烟；点击路径待真机） | UefiScreen 新区块：扫描按钮（运行中禁用+“扫描中…”提示）→ 结果表（Loader/Location/Path/Add 列，兜底命中的 verify 备注以 disabled 文案显示）→ 每行 Add 一键建条目；已存在同路径条目显示“已存在于启动项”；无 ESP / 无命中 / 逐盘探测失败均明示 |
| 3 | 红线不变 | ✅ | 创建走既有 `guardedWrite`（自动备份 → 原生确认框 → 写入）；探测全程只读（`readSectors` + `FatVolume` 只读解析）；槽位 `nextFreeBootNumber`；追加 BootOrder 尾部；HDD 形态 MEDIA_HARDDRIVE_DP 定位节点（`buildHdSpec` 从 applyEdit 抽取共用，GPT 用分区 GUID、MBR 读盘签名、读失败降级 type 0） |
| 4 | 线程模型 | ✅ | `LoaderScan::start`：候选快照（UI 线程）→ worker `std::thread` detach + `shared_ptr<Shared>`（App 磁盘枚举同款，LESSONS #1）；worker 持 `shared_ptr<IDiskAccess>`（App 新增 `sharedDiskAccess()`），App 生命周期外安全；结果/错误增量写入，UI 每帧 `pump()` 收割；worker 内不调用 T_()/I18n（避开翻译字典并发访问） |
| 5 | 门禁 | ✅ | 本地干净目录 gcc：0 错 0 警；ctest 75/593 全过；CI 矩阵 8 格全绿（含 gcc11 下限 ubuntu22.04 + clang14）；Xvfb 冒烟 exit 124 全程存活、日志干净（字体/枚举/0 stalled） |
| 6 | i18n | ✅ | zh_CN.po +11 词条（327→338）：快捷添加区标题/按钮/状态/表头/确认问句；en_US 走 msgid 直通；改 po 后重新 configure（CMAKE_CONFIGURE_DEPENDS 自动） |
| 7 | 文档 | ✅ | 本文档 + M11_PLAN.md + HANDOFF 里程碑表 + ROADMAP 状态行 + CMakeLists 尾注 `M11` 定义 |
| 8 | 版本 | ✅ | project VERSION → **1.1.0**（commit `release: 1.1.0`；tag/发布由用户确认后执行） |

## 2. 实现要点（对账 ROADMAP §3）

- **识别规则**（`core/uefi/LoaderPresets.cpp`，纯逻辑无 OS 调用）：

| 加载器 | 主路径 | 兜底/确认 | 写入描述 |
|---|---|---|---|
| systemd-boot | `\EFI\systemd\systemd-bootx64.efi` | `BOOTX64.EFI` 仅当无其他命中（备注 verify） | `Linux Boot Manager` |
| Limine | `\EFI\LIMINE\LIMINE.EFI` | `BOOTX64.EFI` 需 `limine.conf`/`limine.cfg`（根 / `\EFI\LIMINE\` / `\EFI\BOOT\`） | `Limine` |
| rEFInd | `\EFI\refind\refind_x64.efi` | — | `rEFInd Boot Manager` |

- 描述串保持 bootctl 等工具链官方拼写（固件侧识别），**不做 i18n**；UI 展示串才进 po。
- **单一来源清理**：ESP GUID 判定抽取为 `DiskInfo::isEspTypeGuid()`，替换 UefiScreen / EspFileDialog 两处重复的 `kEspTypeGuid` 局部实现。
- 扫描探测路径约定反斜杠分隔（`FatVolume::findEntry` 大小写不敏感，\ 与 / 均可，契约测试锁定反斜杠形态）。

## 3. 设计决策与偏差

1. **BOOTX64 兜底语义按 ROADMAP 原文实现**：“systemd-boot（备选 `\EFI\BOOT\BOOTX64.EFI`）”。开发中测试曾按“绝不报告”断言写（宁缺勿错解读），与规格冲突后**修正测试对齐规格**：BOOTX64 单独存在 → systemd 兜底命中 + verify 备注（确认框展示描述+路径供人工把关）。Limine 侧歧义红线不变（无 limine 配置证据不报告）。
2. **扫描工作线程而非 UI 线程同步探测**：EspFileDialog 先例是 UI 线程同步读（单分区、用户主动选择）；快捷添加一次扫所有 ESP，病盘可能卡死 UI → 采用 App 枚举的 worker+收割模式（本质差异：范围从"一个分区"扩大到"全部 ESP"）。
3. **一键建不预填表单**：Add 直接走 guardedWrite（备份+确认框中展示 Boot#### 名/描述/路径/位置）；描述为 preset 固定值，如需改描述走既有 Add entry 表单。
4. **Windows 侧**：新增代码全部为可移植 C++20（core 纯逻辑 + 既有 UI 件 + std::thread），MSVC 编译待 Windows Agent 回归（HANDOFF_WIN 流程，不阻断 M11）。

## 4. 遗留事项

- [ ] **真机 UEFI 验证**：真 ESP 扫描命中 systemd-boot/Limine、一键建条目、固件启动新条目（需 root；写前备份+确认框路径核对）。
- [ ] Windows Agent 回归（MSVC 0 错 0 警 + ctest）。
- [ ] 图标定稿（沿用 M10_ACCEPTANCE §5 遗留，1.1.0 仍为占位图标）。
- [ ] 可选扩展：更多加载器 preset（如 XorBoot/refind+drivers 变体）、扫描结果“预填到表单”入口（当前一键直达）。
