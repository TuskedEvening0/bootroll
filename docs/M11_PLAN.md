# M11 实施计划（引导器快捷添加）

> 前置阅读：[ROADMAP.md](ROADMAP.md)（§3 M11 规划）→ [PLATFORM_SEAMS.md](PLATFORM_SEAMS.md)（两阶段枚举 + efivarfs 契约）→ [LESSONS.md](LESSONS.md)（#1/#12/#17 红线）。
> 里程碑定义（根 CMakeLists.txt 尾注）：`M11 : loader quick-add - systemd-boot / Limine (+rEFInd) ESP scan + Boot entry presets`。
> 版本线：M11 开发期保持 1.0.0；完成后按用户指示 bump **1.1.0**。

## 0. 目标与验收（先看这个）

**目标**：UEFI 页新增"快捷添加"：一键扫描所有 ESP，识别 systemd-boot / Limine / rEFInd 加载器，按预设描述一键创建 Boot#### 条目（走既有 备份+确认 写路径）。

**验收清单（全部满足才算 M11 完成）**：

1. **核心纯逻辑** `core/uefi/LoaderPresets`：`detectEspLoaders(exists)` 按 ROADMAP §3 规则判定，doctest 覆盖（≥10 断言组）：
   - systemd-boot 主路径命中；`BOOTX64.EFI` 仅在无其他证据时作为 systemd 兜底（备注 "verify"）；
   - Limine 专目录命中；`BOOTX64.EFI` + `limine.conf`/`limine.cfg` 证据 → Limine 命中；`BOOTX64.EFI` 单独存在 → **不命中**（歧义红线）；
   - rEFInd 命中；多加载器共存 → 多命中（每 preset 至多一条）；
   - 证据优先级：limine 配置证据 > systemd 兜底猜测。
2. **UI**（UefiScreen 快捷添加区）：扫描按钮 → 工作线程探测（UI 不卡）→ 结果表（引导器/位置/路径/依据）→ 每行"添加"一键建条目；已存在同路径条目给提示；无 ESP / 扫描失败给明示。
3. **红线不变**：创建走 `guardedWrite`（自动备份 + 原生确认框）；探测全程只读（`readSectors`）；Boot#### 槽位 `nextFreeBootNumber`；条目追加 BootOrder 尾部；HDD 形态带 MEDIA_HARDDRIVE_DP 定位节点（复用/抽取 `buildHdSpec`）。
4. **线程模型**：扫描 worker detach + `shared_ptr` 状态（LESSONS #1 / App 枚举同款）；worker 持 `shared_ptr<IDiskAccess>`（App 新增 `sharedDiskAccess()`），App 生命周期外安全。
5. **门禁**：0 错 0 警（gcc 本地 + CI 矩阵 8 格）+ ctest 全过；MSVC 侧源代码层面可移植（纯逻辑进 core/，UI 用既有可移植件）。
6. **i18n**：新增 UI 字符串同步 `zh_CN.po`（en_US 走 msgid 直通）。
7. **文档**：M11_ACCEPTANCE.md + HANDOFF/ROADMAP 状态更新 + CMakeLists 尾注。
8. **版本**：完成后 project VERSION → 1.1.0（用户指示；tag/发布由用户另行确认）。

## 1. 识别规则（ROADMAP §3 落地）

| 加载器 | 主路径 | 兜底/确认 | 写入描述（bootctl 官方拼写，保持工具链识别） |
|---|---|---|---|
| systemd-boot | `\EFI\systemd\systemd-bootx64.efi` | `BOOTX64.EFI` 仅当无其他命中（备注需人工核对） | `Linux Boot Manager` |
| Limine | `\EFI\LIMINE\LIMINE.EFI` | `BOOTX64.EFI` 需 ESP 上存在 `limine.conf`/`limine.cfg`（根 / `\EFI\LIMINE\` / `\EFI\BOOT\`） | `Limine` |
| rEFInd | `\EFI\refind\refind_x64.efi` | — | `rEFInd Boot Manager` |

- 判定输入是"该 ESP 上的路径存在性"回调（`FatVolume::findEntry` 大小写不敏感），纯逻辑可测。
- `BOOTX64.EFI` 歧义处理：limine 配置证据 > systemd 兜底；两者皆无 → 不报告（宁缺勿错）。

## 2. 步骤一：core/uefi/LoaderPresets（纯逻辑）

- `LoaderHit{ id, description, efiPath, note }` + `detectEspLoaders(probe)`；
- ESP GUID 判定抽公共件：`DiskInfo.h` 新增 `isEspTypeGuid()`，替换 UefiScreen / EspFileDialog 两处重复的 `kEspTypeGuid` 局部实现（单一来源）。

## 3. 步骤二：UefiScreen 快捷添加

- `LoaderScan` 状态：候选 = `app.disks()` 里全部 ESP 分区快照；worker 内 `FatVolume`（EspFileDialog 同款扇区对齐 reader）逐盘探测；结果增量进共享 vector，UI 每帧收割；
- 抽取 `buildHdSpec(app, diskIdx, partIdx, ...)`（applyEdit 原逻辑），快捷添加与既有表单共用；
- 每行"添加"：`packLoadOptionFull(Active, preset描述, HD节点, 路径)` → 空闲槽位 → BootOrder 追加 → `guardedWrite`。

## 4. 步骤三：测试 / i18n / 文档 / 版本

- `tests/test_loader_presets.cpp`（map 探针模拟 ESP）；CMake 注册 exe + tests；
- po 补词条 → 重新 configure（CMAKE_CONFIGURE_DEPENDS 自动）；
- M11_ACCEPTANCE.md：逐项验收 + 证据；HANDOFF M11 行、ROADMAP 状态行更新；
- `project(bootroll VERSION 1.1.0)`。

## 5. 提交粒度

1. `core: loader presets for ESP quick-add (M11)`（含 isEspTypeGuid 抽取 + 测试）；
2. `uefi: quick-add - ESP loader scan + one-click Boot#### entry (M11)`（UI + App accessor + po）；
3. `docs: M11 acceptance + milestone status`；
4. `release: 1.1.0`（bump）。

## 6. 风险登记

| 风险 | 缓解 |
|---|---|
| 病盘 ESP 卡住扫描 worker | 后台线程 + 增量结果收割；UI 只显示"扫描中"；影响面=该盘探测本身 |
| 非 root 下 ESP 不可读（open EACCES） | 逐盘错误入结果表；提权横幅已有 |
| `BOOTX64.EFI` 误判 | 证据链保守（无证据不报）；备注提示人工核对 |
| MSVC 未回归 | 新代码全部走 core/ 纯逻辑 + 既有 UI 件；编译期无平台头文件（HANDOFF_WIN 交接后续） |
