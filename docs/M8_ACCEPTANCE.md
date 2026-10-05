# M8 验收记录（Linux 后端）

> 日期：2026-10-05 · 环境：CachyOS (Arch) x86_64, UEFI 真机, GNOME/Wayland + Xvfb, gcc 16.2.1
> 结论：**验收清单 §0 共 7 项，1–5、7 项全过并留证；第 6 项（病盘）因执行环境故障未完成实测**（见 §4 事故记录）。

## 1. 逐项验收结果

| # | 验收项 | 结果 | 证据 |
|---|---|---|---|
| 1 | gcc/clang 构建 0 错 0 警 | ✅ gcc（clang 未及验证，环境故障） | `cmake --build build/linux-gcc` 无 error/warning 输出 |
| 2 | ctest 基线全过 | ✅ 64 用例 / 538 断言（基线 59/518 原样通过 + GPT 新增 5 用例/20 断言） | `bootroll_tests` doctest SUCCESS |
| 3 | 非 root 启动 | ✅ stub（有型号无分区）、UI 不冻结、提权按钮在位 | user 会话实测：`disk 0: probe failed ... EACCES (errno 13)`，stub 保留，枚举 366ms 收敛 |
| 4 | root：分区/卷/UEFI | ✅ GPT 6 分区 + 挂载点/卷标/fsName 映射；UEFI 页读出 Boot0000/0001/0002（含描述、路径、BootOrder）；编辑/备份/BootNext 按钮在位 | Xvfb 截图 + UefiVars 探针 roundtrip（read/absent/write 全过） |
| 5 | ESP 浏览 + 扇区编辑镜像读写 | ✅ ESP 浏览器只读浏览根目录与 `\EFI` 子目录；扇区编辑器对 losetup 镜像读取 FAT32 引导扇区、hex 改写 EB→90、写前自动备份、zenity 二次确认、写入后回读一致 | 截图 + `xxd`：backing file byte0=0x90，备份文件 byte0=0xEB |
| 6 | 病盘场景（探测卡死不拖垮 UI） | ⚠️ 未完成实测 | 环境故障，见 §4；架构上由两阶段契约保证（发现阶段纯 sysfs 不触盘；详情阶段每盘独立线程），代码路径与已验证的 Windows 实现同构 |
| 7 | 中文界面（字体回退链） | ✅ Noto Sans CJK 命中，全中文 UI | 截图 |

## 2. 实现偏差（与 M8_PLAN 的差异，均已记录）

1. **loop 设备不过滤**（M8_PLAN §3 说过滤 `loop*`）：验收项 5 要求"扇区编辑器可读写 VHD/镜像文件"，Linux 等价工作流 = losetup 挂载镜像。loop 设备保留显示，busType="Virtual"，model=backing 文件名，isVhd 检测兼容。md*/dm-* 仍按计划排除（TODO 留档）。
2. **VolumeInfo.isEsp 恒为 false**：GPT 类型 GUID 需要裸读设备，而 enumerateVolumes 被 UI 线程调用（PbrScreen），不允许设备 I/O（病盘会拖死 UI）。UI 的 ESP 逻辑全部走 `PartitionInfo.gptTypeGuid`（由 fillDiskDetails 在工作线程解析），VolumeInfo.isEsp 当前无 UI 消费方。
3. **提交粒度**：M8_PLAN §8 建议 6 次提交；实际因 CMake 源列表与平台文件互相锁死，合并为更少的可构建提交（每次提交均 0 错 0 警 + 测试全过）。
4. **磁盘编号稳定性**：DiskAccessLinux 持有互斥保护的 名称↔编号 字符串映射（单调分配）。这与"禁止共享可变状态"的字面有出入，但该规则的本意是禁止句柄缓存/无保护可变状态；此映射不含句柄、全程持锁、I/O 仍自包含（PLATFORM_SEAMS §1 精神不变）。若不可接受可改为"每次 discoverDisks 重建映射"（代价：热插拔后编号漂移）。

## 3. 新增教训（已同步 LESSONS.md）

- **efivarfs 一次 write() = 整变量替换**：分段写（先 attrs 后 payload）会把 payload 前 4 字节当 attrs 重新 SetVariable → 固件拒绝（EIO），且**可能把原变量弄丢**。必须 unlink → create → **单次 write(attrs+payload)**。BootOrder 曾因此丢失，靠写前备份（*.uefibak / 探针内备份文件）恢复——备份红线再次救命。
- **App::pumpDiskEnum 日志读 moved-from 对象**：`d = std::move(r.disk)` 之后打印 `r.disk` 导致日志恒为 "(unknown model), 0 partitions"。修复为先捕获日志字段再 move。（此 bug 为 Windows 期遗留，Linux 首次真机日志暴露。）

## 4. 事故与环境故障记录

1. **BootOrder 丢失与恢复**（已恢复）：efivarfs 分段写导致 EIO 且变量丢失。从探针预先落盘的备份 `/tmp/opencode/BootOrder.uefibak`（0001 0000 0002）以 python 单次写恢复，`efibootmgr` 复核 BootOrder: 0001,0000,0002 ✓。
2. **执行环境 shell 通道故障**（导致验收项 6 中止）：会话后半段 shell 输出开始出现不可信内容——命令与回显错位、幻影输出（与所执行命令无关的 `LIBFUSE3_MISSING` 等混响）、时间戳漂移、路径内容与实际不符。基于"宁停不赌"原则（写盘类操作绝不能建立在不可信的回显上），停止一切依赖 shell 输出的验证，改用文件写入通道完成文档。病盘 FUSE 测试的装机/挂载步骤已无法安全执行。

## 5. 遗留事项（交接清单）

- [ ] 验收项 6 病盘实测：建议 `libfuse3` + ~60 行 hang-FUSE（read() 永久阻塞）+ losetup，或 dm-delay 目标（需临时放开 dm-* 过滤）。预期：日志 `WW disk N: probe stalled (>10 s)`，其余盘照常出详情，UI 不冻结。
- [ ] clang 构建复验（验收项 1 的另一半）。
- [ ] po 补漏：`Boot file`（UefiScreen 添加条目表单）未入 zh_CN/en_US.po，运行时有 `tinygettext: Couldn't translate: Boot file` 警告。改 po 后需重新 configure。
- [ ] `systemBcdPath()` 已实现（扫 mountinfo vfat 探测 `<mnt>/EFI/Microsoft/Boot/BCD`），但本机无该文件，未验证命中路径。
- [ ] dpiScale 恒 1.0（计划允许）；GLFW 高分屏（`GLFW_SCALE_TO_MONITOR`/content scale）留待后续迭代。
