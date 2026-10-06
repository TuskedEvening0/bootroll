# bootroll 交接总览（M8 - Linux 跨平台）

> **受众**：将在 Linux 环境接手 M8（平台 Linux 后端）的开发 Agent。
> **阅读顺序**：本文 → [ARCHITECTURE.md](ARCHITECTURE.md) → [PLATFORM_SEAMS.md](PLATFORM_SEAMS.md) → [M8_PLAN.md](M8_PLAN.md) → [LESSONS.md](LESSONS.md)（编码前必读）。

## 项目定位

bootroll 是 BOOTICEx64 的净室重写：引导扇区/BCD/UEFI 启动项管理工具。
技术栈 **C++20 + CMake + Dear ImGui**，硬约束见下节。当前 M1–M7 已全部完成并在 Windows 10 真机验证，M8 目标是让同一份可移植代码跑在 Linux 上。

## 里程碑状态

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M1 | BCD 编辑（自研 regf hive 读写 + 编辑器 UI） | ✅ |
| M2 | BCD 专业模式（元素级增删改查） | ✅ |
| M3 | MBR/PBR 处理（NT6 MBR 自研 16 位、PBR 提取/写入） | ✅ |
| M4 | 分区/Grub4DOS | ✅ |
| M5 | 扇区编辑（HexEdit + HexView） | ✅ |
| M6 | LEGAL/打包（内嵌许可、单文件 exe） | ✅ |
| M7 | UEFI 启动项管理（NVRAM 读写 + ESP 文件浏览器） | ✅ |
| **M8** | **Linux 后端（platform/linux + 渲染后端替换）** | **✅ 完成（7/7 全过并留证，含病盘停滞告警实测收尾，见 M8_ACCEPTANCE.md）** |
| M9 | 发行版适配（gcc 11 下限 + 容器构建矩阵 + 运行时差异） | ✅ 完成（矩阵 8 格全绿 + 探针留证，见 M9_ACCEPTANCE.md / DISTRO_NOTES.md；版本 0.2.0） |
| M10 | 打包与发布（容器原生 deb/rpm/arch + AppImage/tar.gz → 1.0.0） | 📋 已规划（ROADMAP.md §2） |
| M11 | 引导器快捷添加（systemd-boot / Limine ESP 扫描 + Boot 条目预设） | 📋 已规划（ROADMAP.md §3，1.0 后） |

后续规划（M9–M11）的决策记录与风险登记：见 [ROADMAP.md](ROADMAP.md)。

## 硬约束（不可违反）

1. **第三方源码库仅 ImGui 一个**（其 submodule 自带 backends 除外）；BCD 引擎自研 regf hive 读写，不用外部注册表库。
2. 引导代码：GRUB4DOS/WEE/Syslinux 开源内置；NT MBR 自研 16 位实现（resources/bootcode/）；NT PBR 采用参考卷提取/用户自备；Plop 闭源不内置不安装，仅保留 MBR 类型检测识别。
3. **写前自动备份 + 二次确认**，测试仅用 VHD/镜像文件（绝不对真盘做破坏性测试）。
4. Windows 上确认弹窗必须用 Win32 原生 MessageBoxW（独立于主窗口之上）。Linux 等价方案需在 M8 决策，见 M8_PLAN.md §5。
5. 渲染：Windows 用 DX11 + WARP 回退；Linux 用 OpenGL3 + Mesa llvmpipe 自动兜底（见 M8_PLAN.md §2）。

## 当前构建与测试基线（交接时刻）

- Windows：`MSBuild build\win-msvc\bootroll.sln`（Release/Debug 双配置 **0 错 0 警**）
- 测试：`build\win-msvc\tests\Release\bootroll_tests.exe` → **59 用例 / 518 断言全过**（doctest，全部为可移植纯逻辑测试，Linux 编译即验证）
- M8 验收同样要求：0 错 0 警 + 测试基线全过 + 真机验证清单（见 M8_PLAN.md §0）

## Linux 构建快速上手

```bash
git clone --recurse-submodules <repo>   # third_party/imgui、tinygettext 为 submodule
cmake -S . -B build/linux-gcc -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux-gcc -j
ctest --test-dir build/linux-gcc
```

注意：根 CMakeLists.txt 目前固定链接 DX11/Win32 系统库并强制 `WIN32` 子系统，M8 第一批改动就是把这些变成按平台条件化（见 M8_PLAN.md §1）。CMakePresets.json 需新增 `linux-gcc` preset。

## 目录地图（仓库相对路径）

```
src/
  main.cpp                    入口：平台工厂 + 窗口/渲染循环编排（Win 版；M8 需条件化）
  app/                        App 编排、I18n、Settings、CrashHandler
  core/                       ★ 全部可移植纯逻辑（无 OS 头文件，直接复用）
    bcd/                      regf hive 解析 + BcdStore/BcdElements（M1/M2）
    bootcode/                 MBR/PBR 打包逻辑（二进制 blob 在 resources/bootcode/）
    disk/                     DiskInfo/PartitionTable/IDiskAccess（★两阶段契约见 PLATFORM_SEAMS.md）
    fat/                      FatVolume：FAT12/16/32 只读解析（ESP 浏览器）
    hexedit/ HexText/         扇区十六进制编辑
    uefi/                     UefiVars：EFI_LOAD_OPTION pack/parse、BootOrder 编解码（纯逻辑）
  platform/                   ★ 平台接缝（M8 的工作面）
    IPlatform.h               平台总接口（含 VolumeInfo）
    IUefiVars.h               UEFI 变量接缝
    win/                      Windows 实现（M8 参照物）
  ui/                         全部 ImGui 界面（可移植；EspFileDialog 有独立顶层窗口先例）
  ...
resources/                    字体/翻译(po)/许可/bootcode blob
tests/                        doctest 单测（59 用例）
third_party/                  imgui、tinygettext（submodule）
LEGAL.md                      净室合规声明
```

## 沟通与工程约定

- 测试失败先查 [LESSONS.md](LESSONS.md) 是否已有同款教训。
- 构建 0 错 0 警是门禁；新增 UI 字符串须同步 `resources/i18n/zh_CN.po` 与 `en_US.po`（msgfmt 不参与构建，tinygettext 直接读 po，嵌入由 cmake/EmbedBinary.cmake 完成，改 po 后需重新 configure）。
- 日志：`bootroll.log`，每行 `YYYY-MM-DD HH:MM:SS [II|WW|EE] message`，首行 `---- boot <ver> (commit <hash>) ----`。
- 代码注释/日志英文，对话与 po 译文中文。

## Linux 构建与运行（M8 完成）

```bash
cmake --preset linux-gcc && cmake --build build/linux-gcc -j
ctest --test-dir build/linux-gcc          # 64 用例 / 538 断言
./build/linux-gcc/bin/bootroll
```

- Linux 侧技术选型（已决策）：GLFW + OpenGL3 渲染；zenity 子进程对话框（确认框/文件框）；pkexec 提权；efivarfs UEFI 变量；sysfs + 裸设备 IO 磁盘层。
- Linux 专属文档：[M8_ACCEPTANCE.md](M8_ACCEPTANCE.md)（验收记录 + 偏差 + 事故 + 遗留清单）、[HANDOFF_M8.md](HANDOFF_M8.md)（**M8 收尾交接：遗留任务、环境现状、复核路径**）。
- 非 root 运行属正常形态：磁盘出 stub + 红字提示 + 提权重启按钮（pkexec）；写 UEFI/扇区需要 root 或 disk 组。
