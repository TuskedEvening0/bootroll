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
