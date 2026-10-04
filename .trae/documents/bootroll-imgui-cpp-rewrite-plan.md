# BOOTICE → ImGui + C++ 跨平台重写计划（项目代号：bootroll）

> 目标：用 **Dear ImGui + C++20** 重写 BOOTICE v1.3.3.2（BOOTICEx64.exe, by Pauly）的全部功能，
> 架构上预留 **Windows 10 / Debian / Arch / RHEL** 跨平台支持，**v1.0 只交付 Windows 10 x64**。
> 渲染后端：**Win32 + DirectX 11**（带 WARP 软渲染回退，保证 WinPE/无显卡环境可用）。
> 硬性要求：**gettext .po 多语言**（tinygettext，跨平台无 libintl 依赖）；
> **Windows 交付静态编译单文件 exe**（/MT，免 VC++ 运行库）。

---

## 0. 现状分析（Phase 1 探索结论）

- 工作目录仅有两个文件：`BOOTICEx64.exe`（1.3.3.2, 2015-02-16, 厂商 www.ipauly.com）和
  `BOOTICEx64.exe.i64`（IDA 数据库）。
- **该 EXE 使用魔改版 UPX 加壳**（PE 中有 `UPX1` 节，但标准 `upx -d 5.0.2` 报
  `CantUnpackException`），导入表只有 11 个模块、几乎全是 LoadLibraryA/GetProcAddress 壳桩，
  静态字符串/IDA 函数分析（当前 .i64 仅识别出 1 个函数）基本不可用。
- 结论：**不依赖二进制逆向来定义需求**，采用"行为观察 + 公开文档"的方式做规格（干净室友好）：
  - Phase 0（实施第一步）：运行原版 BOOTICEx64.exe，对每个界面截图 + 记录控件/文案/行为，
    形成 `docs/spec/*.md` 行为规格，作为"完整复刻"的验收基准。
  - 公开资料：BCD 元素 ID 与结构（微软公开文档/Wiki）、MBR/GPT 布局、GRUB4DOS/WEE/Syslinux
    官方源码与文档、Plop 分发条款。

### 用户已确认的三项决策
| 决策点 | 结论 |
|---|---|
| 渲染后端 | Win32 + DirectX 11（WARP 回退）；Linux 阶段另做 SDL2/X11+GL 后端，UI 层共用 |
| 引导代码来源 | 开源引导代码（GRUB4DOS/WEE/Syslinux 等）随源码内置并附许可证；NT 引导代码：MBR 用自研 16 位实现，PBR 运行时从本机/参考卷提取或用户自备文件 |
| v1.0 范围 | **完整复刻**：BCD 编辑 + MBR/PBR 管理 + 分区管理 + 扇区编辑器 + GRUB4DOS + 更多引导代码类型 |

---

## 1. 总体架构

```
┌─────────────────────────────── UI 层（可移植，纯 ImGui）───────────────────────────────┐
│ ui/MainScreen  ui/BcdEditor(+Professional)  ui/MbrScreen  ui/PbrScreen                 │
│ ui/Grub4dosScreen  ui/PartitionScreen  ui/SectorEditor  ui/About  widgets/HexView...   │
├─────────────────────────────── 核心层（可移植，无 OS 依赖）────────────────────────────┤
│ core/bcd/   Hive(regf 读写)  BcdStore(对象/元素模型)  BcdElements(元素ID表)  Templates  │
│ core/disk/  IDiskAccess(接口)  PartitionTable(MBR+GPT 解析/编辑)  VolumeInfo            │
│ core/bootcode/  BootCodePack  MbrCode(打补丁)  PbrCode  blobs/(嵌入式开源引导代码)      │
│ core/sector/ 扇区缓冲、hex 模型      core/i18n  core/settings                          │
├─────────────────────────────── 平台层（每 OS 一份）────────────────────────────────────┤
│ platform/win/  RenderDX11  Win32MsgLoop  DiskWin(\\.\PhysicalDrive + IOCTL)             │
│                Elevate(UAC)  PlatformWin(系统BCD路径/字体/固件类型/VHD识别)              │
│ platform/linux/ (v1.0 仅目录+接口占位与文档：/sys/block 或 udisks2、LibC 映射)          │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

跨平台规则：
- `ui/`、`core/` **禁止**出现任何 `#include <windows.h>`；平台差异全部经
  `IDiskAccess`、`IPlatform`、`IRenderBackend` 三个接口隔离。
- 字符串一律 UTF-8（CMake 设 `/utf-8`；Win32 局部用宽字符转换函数封装）。
- 所有对磁盘/BCD 的写操作走 core 层事务接口：**先备份 → 写入 → 校验 → 失败回滚**。

---

## 2. 目录与文件规划（新建文件全集）

```
bootroll/
├── CMakeLists.txt                    # 顶层； presets: windows-msvc / linux-gcc(预留)
├── CMakePresets.json
├── cmake/EmbedBinary.cmake           # 引导代码 .bin → C 数组(或 /binary 资源)嵌入
├── third_party/
│   ├── imgui/                        # git submodule: ocornut/imgui (master 分支)
│   │                                 #   core + backends/imgui_impl_win32 + imgui_impl_dx11
│   └── tinygettext/                  # git submodule: tinygettext (Zlib 许可)，纯 C++ 读 .po/.mo，
│                                     #   无需 libintl，Windows/Linux 同一套实现
├── src/
│   ├── main.cpp                      # 入口：IPlatform/IRenderBackend 初始化 → App::Run
│   ├── app/App.{h,cpp}               # 主窗口/页面路由/全局设置/主题/字体管理
│   ├── app/I18n.{h,cpp}              # tinygettext 封装：加载 .po/.mo、运行时切换语言、T_(msgid) 宏
│   ├── app/Settings.{h,cpp}          # 语言、自动备份开关等（bootroll.ini 同目录）
│   ├── ui/MainScreen.{h,cpp}         # 目标磁盘下拉 + 功能入口按钮（仿原版主界面）
│   ├── ui/BcdEditorScreen.{h,cpp}    # BCD 列表/编辑/新建/克隆/删除/上下移/默认项/超时
│   ├── ui/BcdProfessional.{h,cpp}    # 专业模式：元素级增删改（字符串/整数/布尔/设备/列表）
│   ├── ui/MbrScreen.{h,cpp}          # MBR 安装/配置/备份/恢复/安装到文件
│   ├── ui/PbrScreen.{h,cpp}          # PBR 安装/备份/恢复/安装到文件
│   ├── ui/Grub4dosScreen.{h,cpp}     # grldr 安装(MBR/PBR)、参数、菜单编辑
│   ├── ui/PartitionScreen.{h,cpp}    # 分区管理：激活/隐藏/取消隐藏/删除/类型修改
│   ├── ui/SectorEditorScreen.{h,cpp} # 磁盘/文件扇区十六进制编辑器
│   ├── ui/AboutScreen.{h,cpp}
│   ├── ui/widgets/HexView.{h,cpp}    # ImGui 大缓冲 hex 视图（offset|hex|ascii, 跳转/分页）
│   ├── ui/widgets/DiskPicker.{h,cpp} # 磁盘/分区选择下拉(含 VHD 标记、容量、分区布局)
│   ├── ui/widgets/ConfirmDialog.{h,cpp} # 危险操作确认（输入序列号/勾选确认）
│   ├── core/bcd/Hive.{h,cpp}         # regf 注册表 hive 完整解析 + 全量重建写入
│   ├── core/bcd/BcdStore.{h,cpp}     # Objects/{guid}/Elements/{id} 模型, GUID 工具
│   ├── core/bcd/BcdElements.{h,cpp}  # 公开的 BCD 元素 ID/类型/语义表 + 校验
│   ├── core/bcd/Templates.{h,cpp}    # “新建 Win7/Win8/XP(NTLDR)/VHD/WinPE/Linux/Mac/Android”模板
│   ├── core/disk/IDiskAccess.h       # readSectors/writeSectors/flush/geometry/ioctl 抽象
│   ├── core/disk/PartitionTable.{h,cpp} # MBR 分区表解析/编辑 + GPT 解析/编辑
│   ├── core/disk/DiskInfo.{h,cpp}    # 磁盘/卷枚举数据模型
│   ├── core/bootcode/BootCodePack.{h,cpp} # 引导代码包注册表（类型→blob→安装前补丁钩子）
│   ├── core/bootcode/MbrCode.{h,cpp} # MBR 打补丁（写入目标盘/文件前的 DPT 保留、签名校验）
│   ├── core/bootcode/PbrCode.{h,cpp} # PBR 打补丁（BPB 参数回填：簇/扇区数/卷标/隐藏扇区）
│   ├── core/bootcode/blobs/*.cpp     # CMake 生成的嵌入数据: grub4dos_mbr, wee, syslinux_*, ...
│   ├── core/sector/SectorBuffer.{h,cpp}
│   ├── platform/IPlatform.h          # 枚举磁盘/卷、系统BCD路径、固件类型、字体路径、提权重启
│   ├── platform/win/RenderDX11.{h,cpp}   # D3D11 设备 + WARP 回退 + imgui_impl_dx11
│   ├── platform/win/Win32Window.{h,cpp}  # RegisterClass/CreateWindow/imgui_impl_win32
│   ├── platform/win/DiskWin.{h,cpp}  # \\.\PhysicalDriveN, FSCTL 锁卷/卸载, 扇区对齐 I/O
│   ├── platform/win/VolumeWin.{h,cpp}# 卷枚举/盘符/ESP 定位/VHD 识别/文件模式扇区读写
│   ├── platform/win/ElevateWin.{h,cpp} # IsUserAnAdmin + ShellExecuteExW(runas) 重启自身
│   ├── platform/win/PlatformWin.{h,cpp}
│   └── platform/linux/README.md      # v1.0 占位：Debian/Arch/RHEL 的实现路线(udisks2/sysfs)
├── resources/
│   ├── fonts/ NotoSansSC-subset.otf  # 内嵌中文子集(~2-3MB, 覆盖界面用字) + 系统字体优先加载
│   ├── i18n/ messages.pot, zh-CN.po, en-US.po   # gettext 目录（构建时可选编译为 .mo，运行时也可直读 .po）
│   └── licenses/ THIRD_PARTY.md + 各开源引导代码许可证全文（GPL2 等）
├── docs/spec/                        # Phase 0 产出：原版逐屏截图+行为记录(BCD/MBR/PBR/...)
├── tests/
│   ├── CMakeLists.txt                # CTest + doctest
│   ├── test_hive_roundtrip.cpp       # 构造→序列化→再解析→逐字节语义等价
│   ├── test_bcd_store.cpp            # 元素增删改、模板生成、GUID 处理
│   ├── test_partition_table.cpp      # MBR/GPT 解析、激活/隐藏位修改、非法输入
│   ├── test_mbr_pbr_patch.cpp        # BPB 回填/DPT 保留正确性（用样例 blob）
│   └── fixtures/ *.bin *.bcd         # 合成 hive、合成分区镜像（不使用真实系统数据）
└── LEGAL.md                          # 干净室声明、第三方清单、微软引导代码来源策略
```

依赖策略：第三方库**全部静态编译进 exe**（FetchContent/git submodule 引入）：
- `imgui`（含捆绑 stb）：UI；`tinygettext`（Zlib 许可）：读 .po/.mo 的纯 C++ gettext 实现，
  不依赖 libintl，Windows/Linux 同一套；`doctest`（测试头文件）。
- 不用 GLFW/SDL（Windows 直接用 imgui_impl_win32）。
编译器：MSVC v143 (VS 2022)，C++20，`/W4 /utf-8 /permissive-`；x64 单一目标。

### Windows 交付物（静态编译要求）
- 运行时库 **静态链接 `/MT`**（Release），不依赖 VC++ Redistributable；
  CMake 中 `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>`。
- 所有第三方库编译为静态库并静态链接；d3d11.dll/dxgi.dll/user32.dll 等为 Windows 10
  系统组件（系统自带，不算运行时依赖），不做延迟加载以外的特殊处理。
- 交付形式：**单个绿色便携 `bootroll.exe`**（双击即用，无安装器），内嵌字体/图标/.po 翻译；
  可选构建目标 `bootroll_upx.exe`（标准 UPX 压缩发行版，原始 exe 始终保留）。
- 链接检查：构建后用 `dumpbin /dependents` 验证仅依赖系统 DLL（无 VCRUNTIME140.dll/
  MSVCP140.dll 等可再发行组件）。

---

## 3. 关键技术方案

### 3.1 BCD 引擎（core/bcd，全项目最重模块）
- BCD 文件即注册表 hive（regf 格式）。**自研读写**而非 bcd.dll COM，原因：
  (a) 离线任意 BCD 文件/挂载 VHD/WinPE 环境都能用；(b) 天然可移植到 Linux；
  (c) 不依赖未公开 API。
- 读：校验 base block → 遍历 hbin/cell → 构建 key/value 树 → 映射为
  `BcdStore{ objects[{guid}]{ description, elements[{elemId, valueType, data}] } }`。
- 写：**全量重建**（模型 → 新 hive 序列化），BCD 文件一般 < 1MB，简单且安全；
  保存策略：`*.bcd` → 写临时文件 → 备份原件 `.bak` → 原子替换。
- 元素类型只需支持 BCD 实际用到的：REG_SZ / REG_EXPAND_SZ? （BCD 主要 0x22000002 字符串、
  0x22000005/设备、0x22000007 整数、0x22000003 布尔、0x22000006 列表 GUID、REG_MULTI_SZ、
  REG_BINARY），元素 ID 表按公开文档整理进 `BcdElements`。
- 系统 BCD 定位：BIOS → `%SystemDrive%\Boot\BCD`；UEFI → ESP 分区
  `\EFI\Microsoft\Boot\BCD`（枚举卷探测；ESP 无盘符时经 `\\.\HarddiskVolumeX` 直读）。

### 3.2 引导代码（core/bootcode）— 法律边界方案
- **内置（开源）**：GRUB4DOS grldr.mbr + WEE（GPLv2）、Syslinux 3/4/5/6 的 mbr.bin 与
  各文件系统 PBR（GPLv2+，标注版本），Plop Boot Manager（作者许可允许免费分发）——
  全部以源码仓库随附二进制 + `resources/licenses/` 完整许可证与版本说明。
- **NT5.x MBR / NT6.x MBR**：自研 16 位实模式引导程序（NASM 源码进仓库），
  语义公开且简单：读 DPT → 找活动分区 → 装载其首扇区 → 校验 0x55AA → 跳转。
  这样避免分发微软代码，同时"安装 NT MBR"功能 100% 可用。
- **NTLDR PBR / BOOTMGR PBR**：不自研（FAT/NTFS 解析 + 引导复杂），改为三种合法来源，
  安装界面提供下拉选择：
  1. 从本机参考卷提取（任意已装好 XP/Vista+ 引导的卷的 VBR 拷贝，回填 BPB 参数）；
  2. 用户自备 512B/整轨引导扇区文件（原版就有"从文件安装"）；
  3. 若本机 `%SystemRoot%` 可读则自动尝试系统内文件提取。
- 所有"安装到磁盘"都提供"安装到文件"（导出）模式，与原版一致。

### 3.3 磁盘/扇区层（Windows 10）
- 枚举：`SetupDiGetClassDevs(GUID_DEVINTERFACE_DISK)` + `STORAGE_DEVICE_NUMBER` +
  `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX` + `IOCTL_DISK_GET_DRIVE_LAYOUT_EX`（MBR/GPT 判定）。
- 扇区读写：`CreateFileW(\\\\.\\PhysicalDriveN, FILE_FLAG_NO_BUFFERING)`，
  读前 `FSCTL_LOCK_VOLUME`/`FSCTL_DISMOUNT_VOLUME`（对物理盘不可锁时提示占用）；
  写前自动备份目标扇区到文件（可关闭）。扇区大小自适应 512/4096。
- VHD 识别：`IOCTL_STORAGE_QUERY_PROPERTY` BusType/产品串（MSFT Virtual Disk）尽力识别。
- 提权：manifest `asInvoker` 启动；需要写盘/写系统 BCD 时检测
  `IsUserAnAdmin()`，否则 `ShellExecuteExW(runas)` 重启自身并带 `--elevated --restore-session`。

### 3.4 UI/字体
- 单窗口主界面仿原版布局：顶部目标磁盘选择器 + 分组按钮（BCD 编辑 / MBR / PBR /
  GRUB4DOS / 分区管理 / 扇区编辑 / 工具选项 / 关于），页面切换为 ImGui 子页。
- 字体：优先加载系统 `Microsoft YaHei UI`（Win10 必有），失败则用内嵌 Noto Sans SC 子集。
- 多语言：**gettext/.po 工作流**（tinygettext 运行时加载）。所有 UI 文案用 `T_("...")` 包裹
  英文 msgid；`resources/i18n/` 维护 `messages.pot` 模板与 `zh-CN.po`、`en-US.po`；
  支持运行时切换语言、复数/上下文；新增第三方语言只需新增 .po 文件。
  CMake 提供可选 target `update_pot`（有 gettext 工具链时用 xgettext 扫源码，无则手工维护）。
  默认语言 zh-CN（与原版一致）。
- 主题：ImGui 深色主题微调，接近原版紧凑对话框风格；DPI 感知（PerMonitorV2）。

### 3.5 安全设计（写盘类工具的硬性要求）
- 任何写入操作：确认对话框（显示目标盘/扇区/影响）→ 自动备份 → 写入 → 读回校验 →
  失败提供还原；全局设置可关闭自动备份（默认开）。
- 自动化测试**只允许**针对合成磁盘镜像文件（file-backed fixture），
  手工验收用新建 VHD 挂载后操作，禁止 CI/脚本触碰物理盘。

---

## 4. 实施里程碑（每步都可编译运行）

| 里程碑 | 内容 | 验收 |
|---|---|---|
| M0 骨架 | CMake 工程（/MT 静态运行库 preset）、imgui+tinygettext submodule、DX11+Win32 窗口、字体(CJK)、.po i18n、主题、主界面布局、磁盘只读枚举显示 | Win10 启动显示中文主界面，下拉列出真实磁盘/分区；`dumpbin /dependents` 验证无 VC++ 运行库依赖 |
| M1 BCD 编辑 | Hive 读写、BcdStore 模型、单元测试、BCD 列表页、基本编辑、系统 BCD/离线文件打开、新建各类条目模板 | 对真实系统 BCD 只读展示与 bcdedit 一致；在 VHD 副本 BCD 上完成新建/修改/删除并经 bcdedit 验证 |
| M2 专业模式 | 元素级增删改 UI、设备元素编辑、导入/导出条目 | 与 bcdedit/set 各类型值互通一致 |
| M3 MBR/PBR | 磁盘扇区读写、备份/恢复/安装到文件、内置开源引导代码安装、NT MBR 自研版安装、NT PBR 参考卷提取安装、BPB/DPT 补丁与测试 | 在挂载 VHD 上完成全部操作并可用原版 BOOTICE 只读校验结果 |
| M4 GRUB4DOS+分区管理 | grldr 安装(MBR/PBR)/配置、WEE、分区管理(激活/隐藏/删除/类型) | 同上，VHD 验收 |
| M5 扇区编辑器 | HexView、goto、编辑写回、文件模式 | 在镜像文件上验收 |
| M6 收尾 | About/选项/自动备份设置、LEGAI/许可证文档、打包便携单 exe、完整回归清单 | 按 docs/spec 逐屏对照原版通过 |

M7（不在 v1.0）：Linux 后端（platform/linux：/sys/block + 直接 /dev 或 udisks2），
按 M0-M5 接口逐项实现，Debian/Arch/RHEL 无差异点（内核接口一致），打包 .deb/AUR/RPM。

---

## 5. 风险与对策
1. 原版加壳导致无法静态比对 → 用行为规格（截图）+ 真机/VHD 对照验收。
2. regf 写入正确性风险 → 全量重建策略 + 大量往返单测 + 用第三方 hivex/python-registry 做
   交叉验证（仅开发期工具，不进产品）。
3. NT PBR 无法自研 → 已定"提取/自备"方案，UI 中明确标注来源与适用性。
4. 微软引导代码版权 → LEGAL.md 固化策略：不分发微软二进制；文档中禁止从原版 BOOTICE 提取引导代码入库。
5. D3D11 在 WinPE 缺驱动 → 创建设备失败时回退 D3D_DRIVER_TYPE_WARP（纯软件渲染）。
6. 误写真实磁盘 → 默认自动备份 + 二次确认 + 测试规范（仅 VHD/镜像）。

---

## 6. 明确不做（v1.0）
- Linux/Debian/Arch/RHEL 的可运行后端（仅接口占位与路线文档）。
- 32 位 Windows、Windows 7/8。
- GPT 特有引导（如 \EFI\Boot 修复）之外的 UEFI 变量操作。
- 任何形式的原版二进制资源直接复制（含从原 EXE 解包提取引导代码）。
