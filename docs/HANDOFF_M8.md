# M8 交接文档（Linux 后端 → 下一任）

> 写于 2026-10-05 会话收尾。受众：接手 M8 收尾/验收的 Agent 或开发者。
> 配套阅读顺序：本文 → [M8_ACCEPTANCE.md](M8_ACCEPTANCE.md)（验收记录）→ [LESSONS.md](LESSONS.md)（#17-#20 为 M8 新增）→ [HANDOFF.md](HANDOFF.md)（项目总览）。
> **注意**：上一会话后期执行环境的工具通道出现过内容失真（详见 §3），本文所引证据均来自失真前后的可信通道复核；§5 列出建议人工复核的清单。
>
> **✅ 收尾完成（2026-10-05 晚，同日续会）**：§4 清单之 1（root 停滞告警实测，特权容器 root 会话 + hangfs 30s 门限）、2（clang 复验，修复 5 警后 0/0 + 64/538 全过）、3（文档复核：发现并回滚一次工具通道幽灵编辑，复核以 git diff 为准）、4（本文档改动已提交）均已完成；hangfs 已入库 `tests/tools/`（含 README，构建/清理红线见该目录）。详见 [M8_ACCEPTANCE.md](M8_ACCEPTANCE.md) §3.2。遗留仅剩 §4.5/§4.6 低优先级项。

## 1. 交接快照

- **状态**：M8（Linux 后端）实现完成；验收清单 7 项中 **1–5、7 全过并留证**，第 6 项（病盘）user 侧全过、root 侧停滞告警实测中断（环境故障，见 §3/§4）。
- **提交链**：`5b6ad27`（M1-M7 基线）→ `d611e67`（M8 平台后端+CMake）→ `2e5979c`（验收记录+教训+po）→ **未提交**：`docs/M8_ACCEPTANCE.md` 终版、`docs/LESSONS.md` #17-#20、本文档。
- **构建/测试基线（gcc，已复核）**：
  ```bash
  cmake --preset linux-gcc && cmake --build build/linux-gcc -j   # 0 错 0 警
  ctest --test-dir build/linux-gcc                                # 64 用例 / 538 断言全过
  ```
- **技术选型（已决策，勿翻案）**：GLFW + OpenGL3 渲染；zenity 子进程对话框（确认框/文件框，fail-closed）；pkexec 提权；efivarfs UEFI 变量（单次写语义）；sysfs 发现 + 裸设备 IO 磁盘层；loop 设备保留显示（镜像/VHD 工作流）。

## 2. 已验证完成事项（证据要点）

| 验收项 | 结果 | 关键证据 |
|---|---|---|
| 1 gcc 0 错 0 警 | ✅ | 构建无 error/warning（clang 复验遗留，见 §4） |
| 2 测试基线 | ✅ 64/538 | doctest SUCCESS；**用户侧曾见 hive 失败 = 与重建管线撞车（链接中途二进制），复跑即绿** |
| 3 非 root 启动 | ✅ | user 会话：`EACCES (errno 13)` → stub 保留、枚举 416ms 收敛、UI 存活、提权按钮在位 |
| 4 root 分区/卷/UEFI | ✅ | GPT 6 分区+卷映射；UEFI 页 Boot0000/0001/0002；UefiVars 探针 read/absent/write roundtrip 全过 |
| 5 ESP 浏览 + 镜像扇区读写 | ✅ | ESP 浏览器浏览根/`\EFI`；losetup 镜像 hex 改写 EB→90：备份(0xEB)→zenity 确认→写入→回读(0x90) 一致 |
| 7 中文界面 | ✅ | Noto Sans CJK 命中（`/usr/share/fonts/noto-cjk/`） |

期间修复的关键缺陷（均已进提交）：
- **efivarfs 分段写丢变量**（BootOrder 曾丢失，靠写前备份恢复）→ `UefiVarsLinux::write` 改为 unlink → create → **单次 write(attrs+payload)**（LESSONS #17）。
- **App::pumpDiskEnum 打印 moved-from 对象** → 日志恒 "(unknown model), 0 partitions"（LESSONS #18）。
- 可移植性：`localtime_s`→`core/util/LocalTime.h`；`BcdStore` FileMode 统一；缺失 `<cstring>` 等。

## 3. 上一会话的环境故障（重要：影响信任边界）

会话后期执行环境（Agent 工具通道）出现**渐进式失真**：

1. shell 回显错位、幻影输出（与所执行命令无关的混响）；
2. 文件读取返回与请求路径/内容不符的伪造内容（含用真实 git 哈希包装的假日志）；
3. 编辑工具返回与请求不符的结果。

**应对**：当时以"写文件→读回校验"维持了一段可信操作，失真扩散后立即停止一切工具操作。会话恢复后（本次）已复核：
- 通道一致性 ✓（echo/写读往返/git 状态均正常）；
- 停摆前写入的两份文档**完好无损**（`M8_ACCEPTANCE.md` 71 行、`LESSONS.md` 60 行，内容与预期一致）；
- 环境无残留 bootroll/hangfs 进程，`/tmp/hangmnt` 已卸载。

**给下一任的纪律**：若再遇回显可疑，立即切换"写文件→读文件"通道并交叉核验；涉及 root + 真实磁盘的操作，凡回显可疑一律停止。

## 4. 遗留任务清单（按优先级）

1. **验收项 6 收尾（root 停滞告警实测）**——工具与步骤全部就绪：
   ```bash
   # hangfs 已编译好：/tmp/opencode/hangfs（源码 /tmp/opencode/hangfs.c，30s 门限版）
   fusermount3 -uz /tmp/hangmnt 2>/dev/null; mkdir -p /tmp/hangmnt
   /tmp/opencode/hangfs /tmp/hangmnt &          # 挂载（首次 open 起 30s 后读取永久挂起）
   losetup -f --show /tmp/hangmnt/blank.img     # → /dev/loopN（30s 内完成，勿超时）
   sleep 35                                     # 等门限关闭
   rm -f build/linux-gcc/bin/bootroll.log
   DISPLAY=:99 ./build/linux-gcc/bin/bootroll & # Xvfb 或真实会话均可（root）
   sleep 15; cat build/linux-gcc/bin/bootroll.log
   #   预期：WW disk N: probe stalled (>10 s)
   #         disks enumerated: M in ... (1 stalled)，其余盘照常出详情，UI 不冻结
   # 清理（顺序！）：kill -9 <hangfs_pid> → losetup -d /dev/loopN → fusermount3 -uz /tmp/hangmnt
   ```
   ⚠️ 三条红线（LESSONS #19/#20）：清理用显式 PID 或 `pkill -x`，**绝不用宽模式 `pkill -f`**（会匹配执行它的 shell 自身）；hangfs 读线程死循环对 SIGTERM 免疫，必须 `kill -9`；losetup 的探测会跨过秒级短门限，门限低于 30s 会把 attach 卡进 D 状态。
2. **clang 构建复验**（验收项 1 另一半）：`cmake --preset linux-gcc` 换 `-DCMAKE_CXX_COMPILER=clang++` 或加独立 preset；确认 0 错 0 警。
3. **文档人工复核**：`M8_ACCEPTANCE.md`/`LESSONS.md` 停摆前最后一刻写入，本次已初步复核无误，但建议人工再过目一遍（尤其 §3.3 清理命令与 §2 证据表）。
4. **提交未入库改动**：`docs/M8_ACCEPTANCE.md`、`docs/LESSONS.md`、本文档（`git add docs/ && git commit`）。另外 `third_party/tinygettext` 子模块有本地脏状态（在树内构建产物），无需提交，可 `git submodule status` 核对。
5. **低优先级遗留**：`systemBcdPath()` 命中路径未验证（本机无 `<vfat>/EFI/Microsoft/Boot/BCD`）；dpiScale 恒 1.0（计划允许），GLFW 高分屏（`GLFW_SCALE_TO_MONITOR`/content scale）留待后续；md*/dm-* 设备支持仍为 TODO（M8_PLAN §3）。
6. **Windows 回归（可选）**：本里程碑改动了共享代码（LocalTime.h、BcdStore FileMode、App::pumpDiskEnum 日志顺序、UefiScreen 缓冲、Grub4dosScreen 路径分隔符），MSVC 侧建议跑一次 0 错 0 警 + 测试确认无回归。

## 5. 环境现状（交接时刻）

| 项 | 状态 | 处置建议 |
|---|---|---|
| Xvfb :99（pid 21229） | 仍在运行 | 复用即可；不要可 `kill 21229` |
| /dev/loop0 → /tmp/opencode/test.img | 已挂载（64MB FAT32 测试镜像） | 扇区编辑/ESP 测试资产；可留可 `losetup -d /dev/loop0` |
| /tmp/hangmnt | 未挂载 ✓ | hangfs 复测时按 §4.1 重建 |
| /tmp/sicktest/bootroll | user 可执行副本（含其 bootroll.log） | user 会话测试资产 |
| /tmp/opencode/{hangfs.c,hangfs,test.img,BootOrder.uefibak,m8probe*.cpp,uefiprobe.cpp} | 探针与工具 | hangfs/探针可直接复用；源码亦可入库 tests/tools（建议） |
| 实体病盘 /dev/sda（U391 USB 238G） | 用户接入；实测**非挂起型**（读毫秒级成功、返回全零） | 可用于"空表盘不拖累枚举"演示；停滞告警须用 hangfs |
| 宿主 UEFI 变量 | 已恢复 BootOrder=0001,0000,0002（efibootmgr 复核） | 无需处理 |

## 6. 关键文件地图（M8 新增/改动）

```
src/platform/linux/            ← M8 工作面（全部新增）
  PlatformLinux.{h,cpp}        ← IPlatform 实现：zenity 对话框、pkexec、字体链、
                                 /proc/self/exe 便携路径、systemBcdPath、firmwareType
  DiskAccessLinux.{h,cpp}      ← 两阶段磁盘枚举（sysfs 发现 / 每盘线程详情）+ 裸 IO +
                                 MBR/EBR/GPT 布局 + 稳定编号映射（互斥保护，无句柄缓存）
  VolumeLinux.{h,cpp}          ← mountinfo 解析、挂载点即 driveLetter、statvfs、
                                 /dev/disk/by-label 反查、attachVolumeData
  UefiVarsLinux.{h,cpp}        ← efivarfs：attrs 前缀布局、unlink+create+单次写、errno 直通
  GlfwWindow.{h,cpp}           ← GLFW 窗口（尺寸/最小化/事件泵）
  RenderGL.{h,cpp}             ← ImGui OpenGL3 后端封装（退出顺序红线见 LESSONS #2）
  SysFs.{h,cpp}                ← 无状态 sysfs 工具（发现/过滤/型号/总线/尺寸/分区表）
src/core/disk/PartitionTable.* ← 新增 GPT 解析（头/表 CRC32 校验、GUID 混端序、NUL 截断名）+ crc32
src/core/util/LocalTime.h      ← localtime_s/r 可移植包装
src/main.cpp                   ← 双平台入口（wWinMain / main）
src/platform/win/*             ← 未动（对照物）
CMakeLists.txt / CMakePresets.json ← 平台条件化 + linux-gcc{,-debug} preset
tests/test_partition_table.cpp ← GPT 5 用例
```

临时/测试资产（不在仓库）：`/tmp/opencode/hangfs{,.c}`、`/tmp/opencode/m8probe*.cpp`、`/tmp/opencode/uefiprobe.cpp`、`/tmp/opencode/test.img`、`/tmp/sicktest/`、`/tmp/opencode/BootOrder.uefibak`。建议将 hangfs.c 与两个探针整理进 `tests/tools/`（或 `tools/`）入库，便于复现验收。

## 7. 真机复核快速路径（新环境 10 分钟）

```bash
git clone --recurse-submodules <repo> && cd bootroll
cmake --preset linux-gcc && cmake --build build/linux-gcc -j
ctest --test-dir build/linux-gcc                      # 64/538
./build/linux-gcc/bin/bootroll                        # 非 root：stub + 提权按钮
sudo ./build/linux-gcc/bin/bootroll                   # root：全量详情 + UEFI 页
# UEFI 探针（可选，读路径无风险；写 roundtrip 会先落备份）
```
