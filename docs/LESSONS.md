# 教训与红线（编码前必读）

来自 M1–M7 开发与真机取证的实际事故。接手 Agent 在写 Linux 后端时逐条对照；调试卡壳时先来这里找同款。

## 线程与生命周期

1. **磁盘枚举 = 两阶段 + 每盘线程**（2026-10-05 定型，接口已按此设计）
   - 病史：某 USB 盘卡内核 → 一次性全量枚举从"卡 2.5 分钟"恶化到"无限期不完成"，表现为"检测不到磁盘 + 浏览按钮灰 + 盘列表全空"（日志只有 `disk enumeration started` 没有 `disks enumerated`）。
   - 契约见 PLATFORM_SEAMS.md §1。Linux 实现若图省事做成"一次遍历全量返回"，等于复刻这个事故。
   - worker 全部 `detach` + `shared_ptr` 状态对象（`src/app/App.h` 的 `DiskEnumState`）；**永不 join** 卡死线程（每病盘泄漏 1 线程，进程退出时由 shared_ptr 兜底，不 use-after-free）。

2. **ImGui 后端关停顺序**（Windows 真机 0xC0000005 事故）
   - `RenderDX11::shutdown()` 必须在 `ImGui::DestroyContext()` **之前**显式调用；正确顺序：`renderer.shutdown() → ImGui_ImplWin32_Shutdown() → app.shutdown() → ImGui::DestroyContext()`。
   - Linux 版（GLFW/OpenGL3）必须保持等价顺序，退出崩溃先查这里。

3. **UI 线程永不阻塞**：所有可能有 IO 的操作走后台线程 + 每帧收割（`App::drawFrame()` → `pumpDiskEnum()`）；Windows 上确认弹窗本来就阻塞主线程（原生对话框语义，可接受），但**枚举/扫描绝不行**。

## 字符串 / i18n

4. **`T_()` 宏悬垂指针**：`T_()` 返回临时 `std::string` 的 `c_str()`，只能在同一完整表达式内直接使用（如 `ImGui::Button(T_("..."))` 安全）；**存入数组/变量前必须先拷贝到 `std::string`**，否则悬垂指针逐帧读已释放内存 → UI 文本与垃圾字节交替闪烁（设备类型下拉框闪烁事故）。任何语言平台同款陷阱。

5. **UEFI GUID 抄错两轮**：EFI_GLOBAL_VARIABLE 必须是 `8be4df61-93ca-11d2-aa0d-00e098032b8c`（曾错抄成 93A4 → 只改 Data2 仍错 → 最终整段对照 EDK2 UefiMultiPhase.h 修正）。GUID 错 = 所有变量一律 errno 203，两台真机症状一致。**Linux 实现直接从 `src/platform/IUefiVars.h` 头注释复制，不要手打。**

6. **"变量不存在"≠"系统性失败"**：读失败必须经 `IUefiVars::lastErrorCode()` + `isUefiVarAbsent()` 分流。Linux 用 errno 直通（`ENOENT==2` 恰好与 `ERROR_FILE_NOT_FOUND` 同值，现有判别直接兼容）；`EACCES/EPERM` 是系统性错误 → UI 显示红字 + 提权重启按钮。**不得吞错静默跳过**（M7 曾因此"读不出参数"却无任何提示）。

7. **BCD 字符串一律 UTF-16LE**：经 `utf16leToUtf8/utf8ToUtf16le`（src/core/bcd/Utf16）转换，与真实 BCD 对齐。

## 磁盘 / 文件系统

8. **FAT32 判定用 BPB**（`rootEntries==0 && fatSize16==0`），不能用"簇数 > 65524"——Windows 100MB ESP 只有约 25600 簇会被误判 FAT16。9. **FAT 目录路径规范化**：FAT 规范允许"父=根"的 `..` 表项簇号为 0；连续两次 `..` 曾报 "Empty directory chain"。修复 = 进目录前先规范化（`.` 跳过、`..` 弹层、越界钳到根），listDir 过滤 `.`/`..`。
10. **MEDIA_HARDDRIVE_DP 的规范 subType = 1**（0x03 是 Vendor-Defined）。曾错写 3 且解析只认 3，roundtrip 自洽掩盖了 bug（固件写的真 HD 节点解析不出）。教训：**roundtrip 自洽 ≠ 正确**，解析要认规范值并兼容旧自产值。
11. **编辑保护**：带 HD 定位节点的 UEFI 条目在分区匹配失败（如磁盘拔出）时，从解析值重建 HdPathSpec 再打包，**永不降级为纯路径条目**；signatureType 任意值逐字节还原 signature。
12. **写前自动备份 + 二次确认**是红线：UEFI 变量写前落 `*.uefibak` 文本（hex 为大端数值表示）；所有写/删类操作过原生确认框。测试只用 VHD/镜像文件。

## 构建 / 流程

13. **0 错 0 警 + 测试全过是门禁**；当前基线 59 用例 / 518 断言。MSVC `/W4 /permissive-`，gcc `-Wall -Wextra`。
14. **commit 哈希经 CMake 注入**（`BOOTROLL_GIT_HASH`）：改完首次 commit 后需重新 configure 才会更新日志头/About 显示，"unknown" 不算 bug。
15. **改 po 后需重新 configure**：翻译/字体是 configure 期嵌入（CMAKE_CONFIGURE_DEPENDS 已声明，正常会自动重跑）。
16. **真机取证先于改代码**：M7 读不出条目两轮排查，最后靠日志 + 探针定位是 GUID 笔误而非 Hyper-V/权限。日志格式规范见 HANDOFF.md，遇问题先加日志取证再动手。

## Linux 后端（M8 新增）

17. **efivarfs 一次 write() = 整变量替换**（2026-10-05 真机事故，BootOrder 曾丢失后恢复）
    - 内核对 efivarfs 文件的每次 `write()` 调用都执行一次完整 SetVariable：先写 attrs 头、再写 payload 时，payload 前 4 字节会被当作 attrs 再次 SetVariable → 固件拒绝（EIO），且**变量可能就此丢失**（`No BootOrder is set`）。
    - 正确顺序：`unlink()` → `open(O_CREAT|O_EXCL)` → **单次 write(attrs+payload 合并缓冲)**。`UefiVarsLinux::write` 已按此实现。
    - 事故靠写前备份恢复——LESSONS #12 的备份红线再次救命；UI 写路径的自动备份在 Linux 同样生效（`backup/` 目录已实测）。

18. **日志打印 moved-from 对象 = 恒为空壳**（App::pumpDiskEnum，Windows 期遗留、Linux 首次真机日志暴露）
    - `d = std::move(r.disk)` 之后打印 `r.disk.model/partitions` → 恒为 "(unknown model), 0 partitions"，与 UI 实际显示相矛盾，误导排障两轮。
    - 修复：先捕获日志字段（number/model/partCount）再 move。任何"先 move 进容器、后打日志"的写法都属同类。

19. **pkill -f 自杀**（2026-10-05，同一晚踩了三次）
    - `pkill -f "build/.../bootroll"` 的模式会匹配到**执行它的 shell 自身的命令行**（bash -c 参数里就有这个字符串）→ shell 被自己的清理命令 SIGTERM。
    - 规矩：清理进程一律 `pgrep -x <精确名>` 或显式 PID kill；pkill -f 只允许在排除了自匹配的上下文中使用。

20. **FUSE 模拟病盘三件事**（hangfs 实测经验）
    - **direct_io 必须开**：否则读走页缓存，永远打不到 FUSE 守护进程，"挂起"形同虚设。
    - **时间门限放宽到 30s**：losetup 的探测序列（头部 + 多处 offset 探测）会跨过秒级短门限，把 attach 自己卡进 D 状态。
    - **读线程死循环 = 守护进程 SIGTERM 免疫**：hf_read 内 `for(;;) sleep(...)` 使 FUSE 请求永不返回，挂起的 dd/losetup 进 D 状态（SIGTERM/timeout 均无效）；唯一恢复 = `kill -9` FUSE 守护进程，让内核读以 EIO 唤醒。

21. **libfuse3 的 direct_io 落点随版本漂移**（2026-10-05 复验 hangfs 时连续三败）
    - **不是挂载选项**：fuse3 不认 `-o direct_io`（fuse2 遗留写法，报 `unknown option(s)`）。
    - **也不是连接能力位**：3.18 头文件已无 `FUSE_CAP_DIRECT_IO`（fuse2→3 迁移文档提过它，别照抄）。
    - **正解：`init(conn, cfg)` 回调里 `cfg->direct_io = 1`**（`struct fuse_config` 字段；init 签名在 3.x 各版本间还有 `void*` 返回值差异，以本机头文件为准）。
    - 附：`fuse_opt_insert_arg` 作用在 `FUSE_ARGS_INIT` 的**借用 argv** 上会触发 assert（`args->allocated==0`）；要改 argv 就自己 strdup 整份（`fuse_opt_free_args` 会逐项 free，借用项会 double-free）。

## CI / 发布（M10→1.0.0 新增）

22. **git 索引执行位可与本地文件系统脱钩**（2026-10-07，v1.0.0 首跑 `Permission denied` exit 126）
    - `scripts/*.sh` 本地都是 755（本地跑管线一直正常），但 git 索引里全是 100644 → CI 上 `./scripts/package.sh` 直接 126。
    - 规矩：可执行脚本入库后用 `git ls-files -s scripts/` 对账模式位（`100755`）；修复用 `git update-index --chmod=+x`，`chmod +x` 在 core.filemode=false 的环境里不进索引。

23. **版本 bump 前对账所有硬编码版本位**（同日，两处会让 1.0.0 出包失败的潜伏问题，0.2.0 期未暴露）
    - arch PKGBUILD `pkgver` 是静态值且 `cp` 进源码目录（与 M10_ACCEPTANCE "自动注入"的记录不符）→ 版本一变 smoke 的 `pacman -U dist/bootroll-${VER}-…` 就找不到文件。修复：`build-arch.sh` sed 注入（与 rpm 同规则）。
    - deb/rpm/arch 三处 smoke `grep "boot 0\."` 硬编码主版本 → bump 到 1.0.0 后日志横幅 `boot 1.0.0` 不再匹配。修复：改 `boot [0-9]+\.`。
    - 规矩：版本号单一来源（project VERSION）指**注入点**；grep/断言/文档示例里的具体版本串也要一并排查。

## Linux 提权（1.1.0 后新增）

24. **pkexec 失败必须留诊断，环境必须回传**（2026-10-07，用户报"以 root 身份重启总提示失败"）
    - 原实现 `pkexec <exe>` 三处哑亏：pkexec 把子进程环境裁到最小集 → 提权实例丢 `DISPLAY`/`WAYLAND_DISPLAY`，GLFW 即死（重启像静默失败）；polkit 0.105（22.04 地板）拒绝非 root 所有的程序（便携版必中）；失败只有裸 `false`，"declined or failed" 无从排查，stderr 丢弃。
    - 修复：`pkexec env` 显式回传显示/会话变量；stderr 进管道分类（root 所有权 / 无认证代理 / Not authorized / 原始 stderr）→ `lastElevateError()` 接缝 + UI 展示 + 日志留证。
    - 规矩：**跨进程子命令的失败路径必须捕获 stderr + 退出码并给出可行动的提示**；提权/对话框类 zenity/pkexec 子进程同理（confirmDialog 的 fail-closed 已是先例）。

27. **提权等待禁止"固定超时 = 已启动"，要等确定信号**（2026-10-11，用户实测"密码框出现后固定 4s 闪退"）
    - 病史：#24 的诊断机制里 wait 轮询带 4s deadline，超时即"视为已启动"返回 true → 原窗口退出。pkexec 快速失败时无害；一旦认证真的挂起等密码，子进程"活着"与"提权实例已运行"无从区分 → 4s 一到原窗口必关，且此时密码框还开着（提权修好后首次暴露，固定复现）。
    - 信号本来就有：pkexec 认证成功后**先 `setreuid` 翻 real uid 再原地 `execv`**，pid 全程不变 → `/proc/<pid>/status` 的 `Uid:` real==0 即"认证完成、实例启动中"。旧版全在猜，读一眼 /proc 就能确定。
    - 修复：轮询顺序 = waitpid（快速失败/取消）→ real uid 翻 0（成功交接）→ 2 分钟兜底（返回 false + 提示语；管道和 pid 交给 detached 线程排水 + 收尸，此后完成认证实例照样自启，取消则原窗口继续可用）。
    - 规矩：**等待外部交互的轮询循环只能以"观测到的状态翻转"或"对方退出"收尾；超时只配当兜底且必须把控制权还给用户**。"活着的子进程"不是成功信号——先问"它在等什么"。

25. **AppImage 提权：FUSE 挂载拒绝 root，永远别把挂载内二进制交给 pkexec**（2026-10-11，用户报 Xfce 下输完密码仍提示 `/tmp/.mount_*/usr/bin/bootroll:权限不够`）
    - 病史：AppImage 的 squashfs 是普通用户 FUSE 挂载的，而 FUSE 挂载默认只放行挂载者本人——**root 也在拒绝之列**。pkexec 验密成功、setreuid 成 root 后 `execv` 挂载内二进制得 EACCES（pkexec 源码：认证先于 exec，失败打印 `Error executing <path>: <errno 文本>`）。#24 的 env 回传全做对了也救不了这一步。
    - 修复：AppImage 模式（`$APPDIR` / 路径含 `/.mount_`）改为 `pkexec env … /bin/sh -c 'exec "$0" --appimage-extract-and-run "$@"' <外层 AppImage>`——/bin/sh root 所有可过 pkexec 校验，外层文件在普通文件系统上 root 可读，extract-and-run 全程不碰 FUSE。`$ARGV0` 必须在本进程内先转绝对路径：pkexec 会把子进程 cwd 切到 /root。
    - 规矩：**setuid/提权链路里凡涉及用户态挂载（FUSE/sshfs/nfs），切 uid 前先想一遍"切过去还读得到吗"**；读不到就换"root 可信入口 + 外层重新拉起"，别硬穿。

26. **魔数偏移是"从分区起算"还是"从结构起算"，探测函数必须带长度守卫**（同日，ext 探测首版测试全跑必炸）
    - 病史：ext 系魔数 0xEF53 在超级块偏移 56（= 分区 0x438），但超级块本体在分区 1024B 处——**单 512B 扇区根本够不到**。照 512B 建的测试夹具按 0x438 写入 = 越界写堆，`probeFilesystemName` 照读 = 越界读。
    - 指纹：单测**单跑全过、合跑段错误/中止、且崩点在完全无关的用例上**（堆布局依赖），gdb 单跑又复现不了——先怀疑夹具/被测函数越界，别信"偶发"。
    - 修复：探测入口统一收 `size` 并对每个魔数做 `size >= 最靠后字段` 守卫（ext 需 0x44C）；Linux 平台层探测一律读 4KB 而非单扇区。
    - 规矩：**新魔数进来先写清楚"偏移的基准点"，夹具缓冲区按最靠后字段定大小**；探测类纯函数一律 `size` 入参 + 守卫出参，禁止隐式假设 512。
