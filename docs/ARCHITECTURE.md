# 架构与模块地图

## 分层

```
┌──────────────────────────────────────────────────────────┐
│ ui/        纯 ImGui 界面（MainScreen 顶栏 + 8 个页签）      │  可移植
│ app/       App 编排：页面路由、磁盘枚举调度、I18n、Settings  │  可移植
├──────────────────────────────────────────────────────────┤
│ core/      纯逻辑：无 OS 头文件、无 ImGui 依赖              │  可移植
├──────────────────────────────────────────────────────────┤
│ platform/  唯一 OS 接缝：IPlatform + IUefiVars + IDiskAccess│  每平台一份
└──────────────────────────────────────────────────────────┘
```

- `platform/IPlatform.h` 是**唯一**的 OS 接缝：Windows 实现 = `platform/win/`（PlatformWin + Win32Window + RenderDX11 + DiskWin + VolumeWin + UefiWin + ElevateWin）。M8 = 新建 `platform/linux/` 提供等价实现，main.cpp 按平台选择工厂 `createPlatform()`。
- core/ui/app 不得出现任何 OS 头文件（`#ifdef _WIN32` 仅允许存在于 platform/win、main.cpp 和 CMake 相关处；IPlatform.h 注释已标注此原则）。

## 关键可移植模块（Linux 直接复用，禁止重写）

| 模块 | 内容 | Linux 侧用途 |
|---|---|---|
| `core/bcd/Hive` + `BcdStore`/`BcdElements` | 自研 regf hive 读写、BCD 对象/元素编解码、UTF-16LE 转换（`utf16leToUtf8`/`utf8ToUtf16le`） | 原样复用 |
| `core/disk/PartitionTable` | **纯逻辑 MBR/GPT 分区表解析**（输入若干扇区字节） | DiskAccessLinux 读 /dev/sdX 前几扇区后交它解析 |
| `core/fat/FatVolume` | FAT12/16/32 只读解析，构造时传 `FatVolumeReader` 回调供扇区；FAT32 判定用 BPB（rootEntries==0 && fatSize16==0），不能用簇数 | ESP 浏览器复用 |
| `core/uefi/UefiVars` | EFI_LOAD_OPTION pack/parse（含 MEDIA_HARDDRIVE_DP 42B 节点）、BootOrder 编解码、`*.uefibak` 文本备份 | UefiVarsLinux 之下原样复用 |
| `core/bootcode/BootCode`/`PbrCode` | MBR/PBR blob 打包（blob 资源内嵌） | 原样复用 |
| `core/hexedit/HexEdit` + `core/util/HexText` | 扇区编辑 | 原样复用 |

## App 层线程模型（M8 必须遵守的既有契约）

`app/App.cpp` 负责磁盘枚举调度，UI 线程永不阻塞：

- `App::refreshDisks()`：起后台线程调用 `IDiskAccess::discoverDisks()`（**契约：此方法绝不碰媒体、绝不阻塞**），随后**每盘一个 detach 线程**调用 `fillDiskDetails(DiskInfo&)`（允许无限期阻塞——病盘只拖死自己）。
- `App::pumpDiskEnum()`：UI 每帧收割完成结果；stub 先显示、详情按 `DiskInfo::number` 原地替换；`generation` 计数丢弃被取代轮次的迟到结果；>10s 打 `WW disk N: probe stalled`。
- `main.cpp` 的渲染循环每帧调 `App::drawFrame()` → `pumpDiskEnum()`。Linux 的 main 循环必须保持同样的每帧节奏（ImGui NewFrame/Render + pumpDiskEnum）。
- 实现方注意：`fillDiskDetails` 会被**并发**调用（每盘一线程），不得持有/复用任何共享句柄或全局可变状态；发现/详情都通过 shared_ptr 状态对象与 UI 线程通信（详情见 `src/app/App.h` 的 `DiskEnumState`）。

## 数据流示例：UEFI 页

```
UefiScreen ──> App::platform()->uefiVars()        (IUefiVars: read/write/remove)
       └──> core/uefi/UefiVars                     (BootOrder 编解码、EFI_LOAD_OPTION pack/parse)
备份: 写 NVRAM 前先落 *.uefibak 文本（hex 为大端数值表示），再原生确认对话框
ESP 浏览: EspFileDialog ──> FatVolume(reader=IDiskAccess::readSectors)
```

## 渲染与入口

- Windows：`main.cpp` → `Win32Window`（Win32 + ImGui_ImplWin32）+ `RenderDX11`（D3D11，WARP 回退）。
- **退出顺序红线**：`renderer.shutdown() → ImGui_ImplWin32_Shutdown() → app.shutdown() → ImGui::DestroyContext()`。任何一端颠倒都会 0xC0000005（详见 LESSONS.md #2）。Linux 版替换为 GLFW/SDL2 + OpenGL3 后端时保持等价顺序。
- CrashHandler（Windows 专用 SEH/dump）：Linux 可先用 signal + backtrace 简化实现，非阻塞项。
