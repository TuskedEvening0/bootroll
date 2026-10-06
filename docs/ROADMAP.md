# ROADMAP（M9–M11 规划与决策记录）

> 写于 2026-10-06，M8 收官后规划。受众：下一任 Agent / 开发者。
> 阅读顺序：本文 → [M9_PLAN.md](M9_PLAN.md)（M9 细化执行计划）→ [M8_PLAN.md](M8_PLAN.md) / [M8_ACCEPTANCE.md](M8_ACCEPTANCE.md)（上一里程碑样板）。
> **状态：M9 已于 2026-10-06 完成验收**（矩阵 8 格全绿 + 字体/fail-closed 探针留证，见 [M9_ACCEPTANCE.md](M9_ACCEPTANCE.md) / [DISTRO_NOTES.md](DISTRO_NOTES.md)；版本 0.2.0）。下一步 **M10 打包与发布**（届时把本文 §2 展开为 M10_PLAN.md）。M11 待 1.0 发布后展开。

## 0. 已定决策（与用户确认，勿翻案）

1. **三段式里程碑**：M9 发行版适配 → M10 打包与发布 → M11 引导器快捷添加（1.0 正式版之后）。
2. **编译器下限 gcc 11**（Ubuntu 22.04；glibc 2.35 即运行时地板）。
3. **打包全容器化**：所有包在对应发行版容器内构建（含 Arch 包），宿主零污染。
4. **产物集合**：`.deb` / `.rpm` / `.pkg.tar.zst` + 便携 `tar.gz` + **AppImage**。
5. **x86_64 only**（bootcode blob 为 x86 语义，不做其他架构承诺）。
6. **版本线**：M9 收尾发 0.2.0；M10 产出 1.0.0-rc → **1.0.0 正式版**；M11 为 1.0 后特性迭代。版本号单一来源 = CMake project VERSION（当前 0.1.0，日志横幅 `boot 0.1.0 (commit …)`）。

## 1. M9 — 发行版适配与可移植性（摘要，细化见 M9_PLAN.md）

**目标**：同一份 core/ui/app/platform 代码在 Debian 系 / RPM 系 / Arch 系容器矩阵全部 0 错 0 警 + 测试全过；运行时差异（字体、对话框、提权）适配并有实测记录。

| 容器 | 编译器 | 说明 |
|---|---|---|
| ubuntu:22.04 | gcc 11 + clang 14 | **基线下限**；系统 cmake 3.22 < presets 所需 3.25，容器内用显式 `cmake -S -B`（根 CMakeLists `cmake_minimum_required(3.21)` 兼容 ✓） |
| debian:12 | gcc 12 | deb 稳定版 |
| ubuntu:24.04 / debian:13 | gcc 13 / 14 | deb 新版 |
| fedora:latest | gcc 14+ | RPM 系 |
| opensuse/tumbleweed | gcc 14+ | RPM 系 |
| Arch（本机已有基线） | gcc 16 / clang 23 | 上限回归（M8 已绿，作对照） |

**关键工作面**：
- gcc 11/clang 14 下限审计（`CXX_STANDARD 20` 在 gcc 11 有缺口：无 `std::format`——现日志代码为 snprintf 风格，确认未用即可）；
- 字体候选链补全（各发行版 Noto CJK 路径，以容器内 `fc-list` 实测为准，不凭记忆硬编码）；
- **内嵌字体 CJK 字形兜底验证**（无字体精简容器）：若内嵌字体不含 CJK 字形，精简系统上中文 UI 会全是豆腐块——这是 M9 最可能出计划外工作的点，尽早试；
- 无 zenity / 无 polkit 的 fail-closed 复验；
- 《发行版适配记录》（编译器/依赖/包名对照 + 实测证据），兼作 M10 包元数据输入。

## 2. M10 — 打包与发布（规划）

**容器分工**：

| 产物 | 构建容器 | 工具 / 要点 |
|---|---|---|
| `.deb` | ubuntu:22.04 | `dpkg-deb`；glibc 2.35 地板 → Debian 12+ / Ubuntu 22.04+ 通吃；AOSC OS 为 dpkg 系（oma），格式直接兼容（best-effort 记录） |
| `.rpm` | opensuse Leap（老）+ fedora（新） | `rpmbuild`（CPack RPM 生成器同样依赖 rpmbuild，躲不开）；单包 or 双包取决于两系依赖包名差异（M9 适配表给出） |
| `.pkg.tar.zst` | archlinux 容器 | `makepkg`（按决策#3，宿主不出包） |
| AppImage | ubuntu:22.04 | `appimagetool` / `linuxdeploy`，`APPIMAGE_EXTRACT_AND_RUN=1` 免 FUSE；runtime 下载 pin 版本 + 容器内缓存；在最低地板上构建 = 最大兼容 |
| tar.gz | 任一容器 | 纯 tar（单文件应用，近零成本） |

**公共项**：
- 安装布局：CMake install 规则（`/usr/bin/bootroll`，FHS）与便携语义兼容（po/许可/引导代码 configure 期内嵌，运行时外部依赖仅系统字体）；
- `.desktop`（过 `desktop-file-validate`，Category=System/Utility）+ 图标——**前置素材：图标资产待用户定**；
- 包元数据：硬依赖只写 glibc / libstdc++ / glfw / libgl；zenity / polkit / CJK 字体进 Recommends / optdepends；
- 发布冒烟：对应容器内安装 → Xvfb 起应用 → 断言日志（字体、枚举、提权按钮）→ 卸载干净；deb 过 lintian、rpm 过 rpmlint；
- 产物带 SHA256SUMS + 发布说明；打 tag。

**里程碑定义草案**（CMakeLists 尾注，开工时追加）：`M10 : packaging — container-native deb/rpm/arch + AppImage/tar.gz + 1.0.0 release`

## 3. M11 —（1.0 后）引导器快捷添加（规划）

本质：**扫 ESP 找已知加载器 EFI → 预填 + 一键建 Boot#### 条目**。

- **systemd-boot**：扫 `\EFI\systemd\systemd-bootx64.efi`（备选 `\EFI\BOOT\BOOTX64.EFI`）；描述固定 **"Linux Boot Manager"**（bootctl 官方描述，保持一致才能被 systemd 工具链识别）。
- **Limine**：候选 `\EFI\LIMINE\LIMINE.EFI`、`\EFI\BOOT\BOOTX64.EFI`；后者有歧义（任何加载器都可能叫 BOOTX64）→ **以 ESP 上存在 `limine.conf`/`limine.cfg` 作为确认条件**；描述 "Limine"。
- 可扩展：rEFInd（`\EFI\refind\refind_x64.efi`，"rEFInd Boot Manager"）。
- 实现地基（全部既有）：
  - ESP 定位走 `PartitionInfo.gptTypeGuid`——注意 M8_ACCEPTANCE §2 偏差：`VolumeInfo.isEsp` 恒 false（UI 线程禁设备 I/O），扫描逻辑走 `fillDiskDetails` 工作线程侧数据；
  - 文件存在性探测用 `FatVolumeReader`（EspFileDialog 同款）；
  - 写条目走 `UefiVars`（LESSONS #17：unlink → create → 单次写）；
  - Boot#### 空闲槽位分配 + 可选 BootOrder 追加 / BootNext；
  - 红线不变：写前备份（LESSONS #12）+ zenity 二次确认 + 仅对镜像/真 ESP 只读探测，写入必须用户显式确认。

**里程碑定义草案**：`M11 : loader quick-add — systemd-boot / Limine (+rEFInd) ESP scan + Boot entry presets`

## 4. 风险登记（全局）

| 风险 | 缓解 |
|---|---|
| gcc 11 编译不过（特性太新） | M9 首个任务就是 22.04 容器试编译，问题早暴露 |
| 内嵌字体缺 CJK 字形 → 精简系统豆腐块 | M9 精简容器实测兜底；不行则换内嵌字体（体积权衡）或包 Recommends CJK 字体 |
| AppImage runtime 下载不可复现 | pin 版本 + 容器内缓存；必要时 vendored runtime |
| openSUSE/Fedora 包名差异导致 RPM 依赖错 | M9 适配表先做包名对照，M10 据此定单/双 RPM |
| AOSC 无官方构建镜像 | 以 ".deb 格式兼容" 记录，best-effort 人工验证 |
| Ubuntu 22.04 cmake 3.22 < presets 3.25 | 容器内显式 `cmake -S -B`（已核实根 CMakeLists 3.21 兼容） |
| 工具通道幽灵编辑（M8_ACCEPTANCE §4 同款） | 全程"写→读→git diff 核验"；root/磁盘操作回显可疑一律停止（LESSONS #19/#20 红线不变） |

## 5. 启动顺序

1. 用户确认当前已知问题修复完毕；
2. M9 按 [M9_PLAN.md](M9_PLAN.md) 开工（同步：CMakeLists 尾注追加 `M9 : …` 定义、HANDOFF.md 里程碑状态表更新）；
3. M10 在 M9 验收后细化（届时把本文 §2 展开为 M10_PLAN.md）；
4. M11 在 1.0 发布后展开。
