# tests/tools — 验收辅助工具

非构建目标，供验收复现使用，不参与 CMake 编译。

## hangfs.c — 停滞盘模拟器（M8 验收项 6）

mini FUSE 文件系统：对外暴露单个 64 MiB 全零文件 `blank.img`；**首次 open 起
30 秒内读取正常**（供 `losetup` 探测完成挂载），**之后所有读取永久挂起**，
用于在 Linux 上模拟"读取挂起的病盘"，触发 bootroll 的 >10s 停滞告警。

```bash
# 构建
cc -Wall -Wextra -o hangfs hangfs.c $(pkg-config fuse3 --cflags --libs)

# 挂载（direct_io 已由程序自动追加，无需手动传）
mkdir -p /tmp/hangmnt
./hangfs -f /tmp/hangmnt &        # 记下 PID
losetup -f --show /tmp/hangmnt/blank.img   # 30s 内完成，勿超时
sleep 35                          # 等门限关闭
# ... 运行被测程序 ...
```

### 清理（顺序！LESSONS #19/#20）

```bash
kill -9 <hangfs_pid>       # 先杀 FUSE（挂起读以 EIO 唤醒，解除 D 状态）
losetup -d /dev/loopN      # 再卸 loop
fusermount3 -uz /tmp/hangmnt  # 最后卸挂载点
```

⚠️ 红线：
- 停止 hangfs **必须 `kill -9 <显式PID>`**；挂起的读请求永不返回。
- 清理进程用显式 PID 或 `pkill -x`，**绝不用宽模式 `pkill -f`**（会匹配执行
  它的 shell 自身命令行）。
- 门限低于 30s 会把 `losetup` 的探测序列卡进 D 状态；如需调整 `GATE_SECS`，
  不得低于 30。
