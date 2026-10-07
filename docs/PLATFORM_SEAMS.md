# 平台接缝规格（Linux 实现必须满足的契约）

三个纯虚接口构成全部 OS 依赖面。以下按"Linux 实现应如何满足语义"逐条给出。所有路径/名称用仓库相对路径。

## 1. `src/core/disk/IDiskAccess.h` —— 两阶段磁盘枚举（2026-10-05 重构，重点）

```cpp
class IDiskAccess {
public:
    virtual ~IDiskAccess() = default;

    // 阶段 1（快，不碰媒体）：发现磁盘 stub。只填 number/model，
    // 绝不打开 /dev/sdX、绝不发任何会 touch 设备的 ioctl。
    virtual std::vector<DiskInfo> discoverDisks() = 0;

    // 阶段 2：填充单盘详情（busType/totalBytes/sectorSize/style/partitions/卷映射）。
    // 会在独立线程中被【并发】调用；允许对病盘无限期阻塞；
    // 查询不了时抛 DiskAccessError（App 层捕获并收割 stub）。
    virtual void fillDiskDetails(DiskInfo& disk) = 0;

    virtual void readSectors(uint32_t diskNumber, uint64_t byteOffset,
                             void* buffer, size_t bytes) = 0;   // 抛 DiskAccessError
    virtual void writeSectors(uint32_t diskNumber, uint64_t byteOffset,
                              const void* buffer, size_t bytes) = 0;
    virtual void flush(uint32_t diskNumber) = 0;                  // fsync 语义
};
```

**背景**：Windows 上一次性全量枚举被一块内核卡死的 USB 盘拖到无限期（4 连启动盘列表全空、浏览按钮灰）。所以拆成两阶段 + 每盘线程。**Linux 同样有病盘卡死问题，必须遵守同一契约。**

### Linux 实现指引（DiskAccessLinux）

- **number 语义**：Windows 用 OS 磁盘索引（PhysicalDriveN）。Linux 没有全局盘号，约定：对 `/sys/block` 过滤后（排除 loop*/ram*/zram*/rom*、md*/dm-* 是否保留见 M8_PLAN §3）按稳定顺序赋予 0..N-1，内部另建 `dev_t(major:minor)` 映射表供 readSectors/writeSectors 用。同一次 App 运行内 number 必须稳定。
- discoverDisks：遍历 `/sys/block/<dev>`：
  - model：`/sys/block/<dev>/device/model`（去尾空格；无则 "(unknown)" 由 UI 兜底，这里可留空）
  - 可选 busType 预填留空，由 fillDiskDetails 填。
- fillDiskDetails（每盘独立，无共享状态）：
  - totalBytes = `/sys/block/<dev>/size` × 512，或打开节点后 `BLKGETSIZE64`；
  - sectorSize = `/sys/block/<dev>/queue/logical_block_size`（默认 512）；
  - busType：按 `device/subsystem`/transport 映射为项目既有字符串：`SATA/USB/NVMe/SCSI/SD/MMC/Virtual`（virtio、loop → "Virtual"，isVhd 检测逻辑依赖 "Virtual" 字样与 model 含 "Virtual"/"VHD"）；
  - 分区表：`open(/dev/<dev>, O_RDONLY)` + `pread` 前 2 扇区（GPT 需再读 L1 备份头与 GPE 数组）→ 交 `core/disk/PartitionTable` 解析。**非 root 时 open 会 EACCES：抛 DiskAccessError 即可**，App 层会保留 stub（有型号、0 B、无分区）——与 Windows 非管理员行为一致，UI 已有降级显示与提权重启按钮；
  - 卷映射：按 M8_PLAN §4 的挂载点映射填 PartitionInfo::driveLetter/label/fsName（Linux 语义：driveLetter 存挂载点或 "sda1" 风格短名，见 §4 决策）。
- readSectors/writeSectors/flush：`open + pread/pwrite + fsync`；writeSectors 需要 root（写前备份/二次确认是 UI 层已有红线）。
- **禁止**：任何全局可变状态、句柄缓存；`fillDiskDetails` 内的打开/读/关必须自包含。

## 2. `src/platform/IPlatform.h` —— 平台总接口

| 方法 | 语义 | Linux 实现指引 |
|---|---|---|
| `createDiskAccess()` | 返回 IDiskAccess 实现 | `DiskAccessLinux`（unique_ptr 裸 new 即可，Windows 版同款风格） |
| `enumerateVolumes()` | 全部挂载卷 → `VolumeInfo` 列表 | 见 §3 |
| `readVolumeFirstSector(driveLetter, out, error)` | 读已挂载卷首扇区（PBR 参考提取用） | 打开挂载点下的块设备节点，或对挂载点路径 `open(O_RDONLY)` 读文件系统首个扇区——用块设备节点保持与 Windows 等价 |
| `openFileDialog/saveFileDialog` | 原生对话框；filter `"描述|*.bin|所有文件|*.*"`；取消返回 "" | 无外部库时最省事：`zenity --file-selection`（子进程，非链接库）或 ImGui 独立窗口。**决策点** M8_PLAN §5 |
| `confirmDialog(title, text)` | **独立于主窗口之上**的原生确认框，阻塞 OK；返回是否确认 | Windows=MessageBoxW。Linux 推荐复用 EspFileDialog 的独立顶层 ImGui 窗口方案（零依赖、已有先例）；备选 zenity --question。M8_PLAN §5 |
| `firmwareType()` | Unknown/Bios/Uefi | `/sys/firmware/efi` 存在 → Uefi；否则 Bios（chroot/容器内亦按此判定） |
| `uefiVars()` | 返回 IUefiVars*（平台层持有，随平台存活） | `UefiVarsLinux`，见 §4 |
| `dpiScale()` | 主屏 DPI/96，下限 1.0 | X11: `Xft.dpi`/GDK_SCALE；Wayland: 由窗口后端处理，可先返回 1.0f |
| `systemBcdPath()` | 系统 BCD 路径，找不到返回 "" | 低优先级：扫 `/proc/mounts` 中 vfat 挂载点探测 `<mnt>/EFI/Microsoft/Boot/BCD`；实现不了先返回 ""（Linux 上以"打开文件"为主路径） |
| `candidateFontPaths()` | CJK 字体路径，优先级降序 | Noto Sans CJK SC（`/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc`、`NotoSansCJKsc-Regular.otf`）、文泉驿微米黑、DroidSansFallbackFull；路径不存在会被 App 层自动跳过并回退内嵌字体 |
| `isElevated()` | 是否已提权 | `geteuid() == 0` |
| `restartElevated(args)` | 提权重启自身；用户拒绝返回 false | `pkexec env <显示环境> /full/path/to/bootroll <args>`（见下方 pkexec 要点）；成功后调用方会 `requestExit()` |
| `lastElevateError()` | 上次 `restartElevated` 失败的原因（"" = 无/不适用；Windows 恒空） | 已知类别返回英文 msgid（po 可译），未知类别返回 pkexec 原始 stderr；UI 在拒绝提示旁展示并写日志 |
| `iniPath()` | 每用户设置文件全路径 | 保持单文件便携原则：`/proc/self/exe` 所在目录 + `bootroll.ini`（Windows 版同款语义，勿用 XDG 改变行为） |
| `logPath()` | 日志文件全路径 | 同上目录 + `bootroll.log` |

### `VolumeInfo` 字段语义（IPlatform.h 内定义）

```cpp
driveLetter       // Windows "C:"；Linux → 约定为挂载点（如 "/mnt/usb1"），
                  // UI 只做展示/拼接，ESP 场景要求为空也可（letter-less ESP）
label / fsName    // UTF-8；fsName = "NTFS"/"FAT32"/...（mountinfo 的 fs type）
totalBytes/freeBytes
diskNumber/partitionNumber  // 回指 IDiskAccess 的 number + 分区号（缺省 0xFFFFFFFF）
isEsp             // 尽力而为：GPT 分区类型 GUID == c12a7328-f81f-11d2-ba4b-00a0c93e0093
```

## 3. `enumerateVolumes()` Linux 指引

- 数据源 `/proc/self/mountinfo`（去重：同一设备多挂载点取第一个）；
- `isEsp`/分区定位：由 fillDiskDetails 已解析出的 GPT 分区表拿 `gptTypeGuid`（`core/disk/PartitionTable` 输出），匹配 ESP GUID 后填 `diskNumber/partitionNumber/isEsp`；
- totalBytes/freeBytes：`statvfs`；
- **避免 Windows 的坑**：Windows 版在每盘线程里全量卷枚举；Linux 实现若把 `/proc/self/mountinfo` 解析做成纯文件读取（不碰设备），可安全地在任何线程调用。

## 4. `src/platform/IUefiVars.h` —— UEFI 变量接缝

```cpp
class IUefiVars {
public:
    virtual bool ensureReady(std::string* error) = 0;          // 一次性就绪检查
    virtual bool read(const std::string& name, std::vector<uint8_t>* data,
                      uint32_t* attrs, std::string* error) = 0;
    virtual bool write(const std::string& name, const std::vector<uint8_t>& data,
                       uint32_t attrs, std::string* error) = 0;
    virtual bool remove(const std::string& name, std::string* error) = 0;
    virtual uint32_t lastErrorCode() const = 0;                // 上次失败的原因码
};
// "变量不存在" 与 "系统性失败" 的判别（core/uefi 之上的 UI 逻辑依赖它）
inline bool isUefiVarAbsent(uint32_t code); // 2 / 203 / 1168 视为不存在
```

### Linux 实现指引（UefiVarsLinux，基于 efivarfs）

- 路径：`/sys/firmware/efi/efivars/<Name>-<GUID>`，GUID 固定 EFI_GLOBAL_VARIABLE `8be4df61-93ca-11d2-aa0d-00e098032b8c`（core/uefi/UefiVars 与 IUefiVars.h 头注释可交叉验证；历史上曾因 GUID 抄错导致所有读取 203，见 LESSONS.md #5）。
- 文件布局：**前 4 字节 = Attributes（uint32 LE）**，其后为 payload。read 时拆出 attrs；write 时写 `attrs(LE) + payload`，attrs = 0x7（NV|BS|RT）。
- `ensureReady`：检查 efivarfs 目录存在（否则报"非 UEFI 环境"）；可读性检查（root 或已授权）。error 文案进 po。
- **lastErrorCode → errno**：Linux 实现把 errno 原样返回。巧合且重要：`ENOENT == 2` 与 `ERROR_FILE_NOT_FOUND` 同值，**现有 `isUefiVarAbsent` 直接兼容**；`EACCES(13)/EPERM(1)/EIO(5)` 等会被 UI 识别为系统性错误并显示红字/提权按钮（App/UefiScreen 逻辑已就绪）。
- **efivarfs 写坑**：内核不允许通过已打开 fd 变长改写已存在变量——正确顺序是先 `unlink()` 再 `create+write`（O_TRUNC 语义不可靠）。write() 内部必须按此实现，否则改 BootOrder 长度变化时失败。
- 变量名映射：UI 传来的 name 是纯名（"Boot0001"/"BootOrder"），实现方负责拼 GUID 后缀。

### pkexec 提权重启要点（1.1.0 修复，原为裸 false 无诊断）

- **环境重置**：pkexec 把子进程环境裁到最小集（旧 polkit 尤甚）——提权实例会丢失 `DISPLAY`/`WAYLAND_DISPLAY`，GLFW 初始化即死，重启表现为"点了没反应"。必须 `pkexec env DISPLAY=… XAUTHORITY=… WAYLAND_DISPLAY=… XDG_RUNTIME_DIR=… DBUS_SESSION_BUS_ADDRESS=… /path/bootroll` 显式回传（存在哪个传哪个）。
- **程序所有权**：polkit 0.105（Ubuntu 22.04 地板）的 pkexec 拒绝执行非 root 所有的程序（tar.gz 解压目录/AppDir 场景必中）；polkit 122+ 已无此检查。诊断文案需引导"系统包安装或 sudo"。
- **无认证代理**：会话没有 polkit agent 时 pkexec 无法询问密码（内建 tty agent 对 GUI 进程不可用）。
- **诊断通道**：fork 子进程 stderr 进管道（O_NONBLOCK，wait 轮询内排水防死锁），4 秒快速退出 → `classifyElevateFailure` 分类 → `lastElevateError()` 供 UI 展示 + `bootroll.log` 留证。超时视为已启动（返回 true）。

## 5. 平台工厂与 main.cpp

`main.cpp` 按平台选择 `createPlatform()`（现仅 Windows）。M8 需将其条件化：Windows 分支保持 `#ifdef _WIN32`，Linux 分支进 `platform/linux/PlatformLinux.cpp`。窗口/渲染后端选型与改造步骤见 M8_PLAN.md §2。
