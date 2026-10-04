# M8 实施计划（Linux 后端）

> 前置阅读：HANDOFF.md → ARCHITECTURE.md → PLATFORM_SEAMS.md → LESSONS.md。
> 里程碑定义（根 CMakeLists.txt 尾注）：`M8 : platform/linux backend + linux-gcc preset activation`。

## 0. 目标与验收（先看这个）

**目标**：同一份 core/ui/app 代码，在 Linux 上以原生感跑起来，磁盘/卷/UEFI 三大平台能力可用。

**验收清单（全部满足才算 M8 完成）**：

1. `cmake --build`（gcc/clang）**0 错 0 警**（-Wall -Wextra 已在根 CMakeLists 非 MSVC 分支开启）；
2. ctest：**59 用例 / 518 断言** 基线全过（全部纯逻辑测试，应原样通过；过不了说明你改坏了可移植层）；
3. 非 root 启动：窗口正常、磁盘列表出 stub（有型号无分区）、UI 不冻结、有"提权重启"按钮（pkexec）；
4. root/sudo 启动：分区表/卷/文件系统正确显示；UEFI 机器上 UEFI 页读出 Boot#### 条目，编辑/备份/BootNext 可用；
5. ESP 浏览器可只读浏览 ESP；扇区编辑器可读写 VHD/镜像文件；
6. 病盘场景：模拟一块卡死的 USB（或读假设备）→ 其余盘正常出详情，日志出现 `WW disk N: probe stalled (>10 s)`，UI 永不冻结；
7. 中文界面正常（字体回退链生效）。

## 1. 步骤一：CMake 条件化 + linux-gcc preset（半天量级）

根 CMakeLists.txt 目前硬编码 Win32：`add_executable(bootroll WIN32 …)`、DX11 后端源、`target_link_libraries(… d3d11 dxgi …)`、MSVC 运行库设置。改造：

- `add_executable` 的 `WIN32` 关键字、`MSVC_RUNTIME_LIBRARY`、`/utf-8` 等仅 MSVC 分支保留；
- 平台源码目录条件化：Windows → `src/platform/win/*` + imgui win32/dx11 backends；Linux → `src/platform/linux/*` + imgui GLFW/OpenGL3 backends（见 §2）；
- 系统库：Windows 分支保持现状；Linux 分支 `pthread`（C++20 std::thread 需要）+ OpenGL + 窗口库；
- `CMakePresets.json` 新增 `linux-gcc` / `linux-gcc-debug` preset（对照现有 win-msvc preset 的字段）；
- 现有 embed_binary 机制（字体/po/许可/bootcode blob）完全平台无关，不动。

## 2. 步骤二：窗口 + 渲染后端（最大决策点，先向用户确认）

现状：`main.cpp` + `platform/win/Win32Window` + `RenderDX11`（ImGui Win32+DX11 官方后端）。

**推荐方案**：GLFW + OpenGL3（imgui 自带 `backends/imgui_impl_glfw.cpp` + `imgui_impl_opengl3.cpp`，随 submodule 已就位，不新增源码库）。系统包依赖：`libglfw3-dev`、`libgl1-mesa-dev`（或发行版等价包）。

- 约束说明：项目硬约束"仅 ImGui 一个第三方库"指 **vendored 源码库**；ImGui 官方 backend 源文件属于 third_party/imgui 仓库本体，系统开发包是构建依赖（类似 pthread），不违背约束精神。**但这是决策点，动手前须获用户确认。** 备选：SDL2（更重）。
- 新增 `platform/linux/RenderGL.cpp`（对照 RenderDX11 的接口形态）+ GLFW 窗口包装（对照 Win32Window：尺寸/DPI/resize 事件）。
- main.cpp 主循环骨架保持：`while (!app.exitRequested()) { pump events; imgui newframe; app.drawFrame(); render; }`；**保持每帧调用 `App::drawFrame()`**（磁盘枚举收割依赖它）。
- 软渲染兜底：Mesa llvmpipe 自动回退，无需代码（对应 Windows WARP）。
- **退出顺序**（LESSONS.md #2）：`renderer.shutdown() → ImGui_ImplGLFW_Shutdown() → app.shutdown() → ImGui::DestroyContext()`。

## 3. 步骤三：DiskAccessLinux（核心工作量）

契约见 PLATFORM_SEAMS.md §1（两阶段 + 并发 + 抛 DiskAccessError）。要点：

- discoverDisks：纯 sysfs，**不 open /dev 节点**；过滤 `loop* ram* zram*rom* fd* sr*`；`md*/dm*`（RAID/LVM）首版可排除，记入 TODO；
- number 映射：0..N-1 稳定顺序 + 内部 dev_t 映射表（懒建，含分区 major:minor）；
- fillDiskDetails：`open(/dev/sdX, O_RDONLY)` 失败（EACCES）→ 抛错保留 stub；分区表交给 `core/disk/PartitionTable`（传 MBR 扇区 + GPT 头/数组扇区）；
- GPT：解析 primary + 备份头二选一容错；ESP GUID `c12a7328-f81f-11d2-ba4b-00a0c93e0093` 供卷层用；
- readSectors/writeSectors/flush：`pread/pwrite/fsync`；对齐由内核处理，无需用户态对齐缓冲（区别于 Windows 的扇区对齐限制，但保留 byteOffset 校验）。

## 4. 步骤四：VolumeLinux（挂载点映射）

- 解析 `/proc/self/mountinfo`，按设备去重；`fsName` 取 fs type 字段（ext4→"EXT4" 等大写化，与 UI 现有展示一致即可）；
- `VolumeInfo.driveLetter` 在 Linux 放**挂载点**（决策：UI 全部只做展示与路径拼接，已确认安全）；ESP 可为空挂载点（未挂载 ESP 不出现，符合"Windows 不挂 ESP"的既有事实）；
- `statvfs` 取容量；`isEsp/diskNumber/partitionNumber` 由 DiskAccessLinux 解析出的 GPT 表回填（实现顺序：先有 §3 再有 §4）；
- readVolumeFirstSector：打开设备节点读 0 扇区（PBR 参考提取）。

## 5. 步骤五：对话框（决策点，须用户确认）

| 用途 | 推荐 | 备选 |
|---|---|---|
| confirmDialog（写前二次确认，红线） | **ImGui 独立顶层窗口**（EspFileDialog 已有独立顶层窗口先例，零依赖） | zenity --question 子进程 |
| 文件对话框 | zenity --file-selection（子进程，不算链接库） | ImGui 自绘文件浏览器（工作量大） |

无论选哪个：确认框语义 = 阻塞 + 独立于主窗口之上 + 返回 bool；文案走 po。

## 6. 步骤六：UefiVarsLinux

见 PLATFORM_SEAMS.md §4（efivarfs 布局、先 unlink 再写、errno 直通 lastErrorCode、GUID 固定 `8be4df61-93ca-11d2-aa0d-00e098032b8c`）。UI 侧（UefiScreen）已按"不存在 vs 系统性错误"分流并内置提权重启按钮，Linux 实现无需改 UI。

## 7. 步骤七：杂项

- candidateFontPaths：Noto Sans CJK SC / WenQuanYi / DroidSansFallbackFull 候选链；
- iniPath/logPath：`/proc/self/exe` 同目录（保持单文件便携语义，不用 XDG）；
- restartElevated：`pkexec`，成功后调用方 requestExit（已有流程）；
- CrashHandler：Linux 用 signal(SIGSEGV/SIGABRT)+backtrace_symbols_fd 简版即可，dump 机制非必需；
- dpiScale：先返回 1.0f，GLFW 高分屏后续迭代；
- 新增任何 UI 文案 → 同步 zh_CN.po / en_US.po。

## 8. 建议实施顺序与提交粒度

1. CMake 条件化（Linux 能编译出"无平台实现"的最小骨架）→ 单独提交；
2. GLFW/OpenGL3 窗口+渲染 → 空 UI 起来 → 提交；
3. DiskAccessLinux + VolumeLinux → 磁盘页/浏览可用 → 提交；
4. UefiVarsLinux → UEFI 页可用 → 提交；
5. 对话框/杂项/字体 → 提交；
6. 真机验收清单跑一遍（§0），附日志。

每步都保持 0 错 0 警 + 测试全过（在 Linux 上跑 ctest）。
