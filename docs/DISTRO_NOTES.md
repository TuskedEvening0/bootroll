# 发行版适配记录（M9）

> 2026-10-06，M9 实测记录。矩阵工具：`scripts/build-matrix.sh`（容器烘焙工具链，逐格 0 错 0 警 + ctest 门禁）。
> 探针：`tests/tools/m9probe.cpp`（字体解析 + fail-closed，见 `tests/tools/README.md`）。
> 本文只收录实测数据；未实测项明确标注"待 M10 实测"。

## 1. 构建矩阵（全部 0 错 0 警 + ctest 64 用例 / 538 断言）

| 容器 | 编译器（实测） | cmake（实测） | GLFW | 结果 |
|---|---|---|---|---|
| ubuntu:22.04 | gcc 11.4.0（**下限**） | 3.22.1 | libglfw3-dev 3.3.6 | ✅ |
| ubuntu:22.04 | clang 14.0.0 | 3.22.1 | 3.3.6 | ✅ |
| debian:12 | gcc 12.2.0 | 3.25.1 | 未记录 | ✅ |
| ubuntu:24.04 | gcc 13.3.0 | 3.28.3 | 未记录 | ✅ |
| debian:13 | gcc 14.2.0 | 3.31.6 | 未记录 | ✅ |
| fedora:latest | gcc 16.2.1 (20260819) | 4.3.0 | glfw-devel 3.5.x | ✅ |
| opensuse/tumbleweed | gcc 16.2.1 (20260929) | 4.4.3 | glfw-devel 3.5.1-2.1 | ✅ |
| Arch 本机（上限回归） | gcc 16.2.1 (20260810) + clang 23.1.1 | 4.4.4 | glfw 3.5.1 | ✅ |

要点：
- **cmake 3.22（22.04）< presets 所需 3.25** → 矩阵统一显式 `cmake -S . -B`（根 CMakeLists `cmake_minimum_required(3.21)` 兼容，presets 仅本机开发用）。
- **GLFW 3.3（22.04/debian12）**：无 `glfwGetPlatform()` → `GlfwWindow::isWayland()` 已带 `GLFW_VERSION` 守卫；`GLFW_SCALE_TO_MONITOR`/content scale 为 3.3 原生特性 ✓；无 Wayland 分数缩放协议（整数缩放可用）。
- GCC 14（debian13，gcc 14.2）`-Wfree-nonheap-object` 假阳性：`UefiVarsLinux::write` 的 reserve+push_back 路径被报 "delete on pointer with nonzero offset [1, PTRDIFF_MAX]"。M9 矩阵首轮出现后未复现，M9 后 CI 上稳定复现 → **按行为不变的重构消除**（blob 定长构造 + 索引赋值，无 push_back），未使用编译旗标。

## 2. 依赖包名对照

**已实测**（矩阵/探针容器内安装验证）：

| 包 | Debian/Ubuntu |
|---|---|
| GLFW 开发 | `libglfw3-dev`（22.04 实测 3.3.6） |
| GL 开发 | `libgl1-mesa-dev` |
| 字体（探针 D 场景） | `fonts-noto-cjk` |
| 无头测试 | `xvfb libgl1 libglx-mesa0 libgl1-mesa-dri`（`--no-install-recommends` 下 Xvfb 可启动、无 CJK 字体） |
| 字体覆盖检查 | `fontconfig`（fc-list/fc-match，探针 B 场景） |
| 反例字体 | `fonts-dejavu-core`（0 zh 覆盖，见 §4 场景 B） |

| 包 | Fedora | openSUSE |
|---|---|---|
| GLFW 开发 | `glfw-devel` | `glfw-devel`（3.5.1-2.1） |
| GL 开发 | `mesa-libGL-devel` | `Mesa-libGL-devel` |

**待 M10 打包时实测**（元数据用，暂勿直接引用）：`zenity`/`zenity`/`zenity`、`policykit-1`/`polkit`/`polkit`、CJK 字体包名（Fedora `google-noto-sans-cjk-ttc-fonts`?、openSUSE `noto-sans-cjk-fonts`?、Arch `noto-fonts-cjk`）、RPM 系 xvfb 包名。

## 2b. 打包阶段实测补充（M10，2026-10-06）

以下包名/机制在打包容器内**实测验证**（scripts/packaging/*）：

| 项 | Debian/Ubuntu | Fedora | openSUSE | Arch |
|---|---|---|---|---|
| 构建辅助 | `dpkg-dev` `lintian` `desktop-file-utils` | `rpm-build` `rpmlint` | `rpm-build` `rpmlint` | `base-devel`(镜像自带) |
| 无头 X | `xvfb`（含 xvfb-run） | `xorg-x11-server-Xvfb` | `xorg-x11-server-Xvfb` | `xorg-server-xvfb` |
| GL 运行时 | `libgl1 libglx-mesa0 libgl1-mesa-dri` | `libGL mesa-dri-drivers` | `Mesa-libGL1` | `mesa` |
| 安装器 | `dpkg-deb`（dpkg 自带） | `dnf install ./x.rpm` | `zypper --non-interactive --no-gpg-checks install ./x.rpm`（unsigned 本地包必须 `--no-gpg-checks`） | `pacman -U` |
| 已避坑 | — | `xvfb` 包名不存在；`which` 命令不存在（用 `command -v`） | `Mesa-libGL1`（大小写敏感）；`xvfb` 包名不存在 | makepkg 禁 root → 容器内建 builder 用户 |

打包产物证据（dist/，SHA256SUMS）：`bootroll_0.2.0_amd64.deb`、`bootroll-0.2.0-1.fc44.x86_64.rpm`、`bootroll-0.2.0-1.x86_64.rpm`(tumbleweed)、`bootroll-0.2.0-1-x86_64.pkg.tar.zst`、`Bootroll-0.2.0-x86_64.AppImage`、`bootroll-0.2.0-linux-x86_64.tar.gz`。
lintian/rpmlint 残留（记录不门禁）：deb 缺 changelog/man page；rpm spelling-error（专有名词）+ LEGAL.md 无 shebang（%license 文件）。

AppImage：linuxdeploy（continuous，x86_64）sha256 `8aea8da0f7f7039d2a2cecb14657d752a222a5e1d3825caeef186c82f751cdd1`——脚本每次构建打印，正式发布前把 URL 换成固定 release tag 并以此 hash 校验。AppImage 内 squashfs 只读 → 便携日志/ini 写入静默失败（预期，进程存活冒烟替代日志断言）。

## 3. 下限审计修复（矩阵首轮发现，均已入库）

1. **`HexEdit.h` 缺 `<cstddef>`**（gcc 12 报 `'size_t' has not been declared`；gcc 11/13+ 因传递包含路径不同而幸免）——已补 include。
2. **GCC 12 `-Wrestrict` 假阳性 ×6**（`"d" + std::to_string(...)` 惯用拼接，"offsets [2, 9223372036854775807]" 荒谬报告；GCC 13 上游修复）→ CMakeLists 对 GCC 12.x 门控 `-Wno-restrict`。
3. **tumbleweed 镜像构建 exit 106**：基础镜像无仓库元数据 → Dockerfile 先 `zypper --gpg-auto-import-keys refresh` 再安装。

## 4. 字体解析（fc 动态 + 静态兜底 + 内嵌三层）实测

解析顺序：`fc-list :lang=zh-cn` 覆盖集 → `fc-match`（'Noto Sans CJK SC:lang=zh-cn' → ':lang=zh-cn'，命中须在覆盖集内）→ 静态路径表 → 内嵌 NotoSansSC 子集。验证场景（探针，`PROBE_RESULT bad=0` 全过）：

| 场景 | 环境 | candidateFonts | 判定 |
|---|---|---|---|
| A | 无 fontconfig、无字体、无 zenity/pkexec | 空 | **fail-closed ✓**（confirmDialog=false、restartElevated=false） |
| E2E | 同 A，Xvfb 跑真应用 | 日志 `font: <embedded NotoSansSC subset>` | **内嵌兜底实证（无豆腐块路径）** |
| B | fontconfig + DejaVu（6 字体、0 zh 覆盖） | 空 | **fc-match 模糊匹配被真覆盖校验拒绝（防豆腐块关键）** |
| D | fonts-noto-cjk 已装 | `opentype/noto/NotoSansCJK-Regular.ttc [face 2]` | Debian 布局动态解析 SC 面 ✓ |
| 主机 | Arch + Noto CJK | `noto-cjk/NotoSansCJK-Regular.ttc [face 2]` | ✓ |

注意事项：
- fc-match 命中文件与静态表可能重复（loadFonts 取首个成功，无影响；已知小瑕疵）。
- 上游 Noto Sans CJK ttc 面序 JP(0), KR(1), SC(2), TC(3), HK(4), Mono…（fc-scan 实证）；静态表按此给 SC=2，动态路径不依赖该假设。
- **内嵌字体为 SC 子集**：zh_CN UI 常用字形可用；极端生僻字可能缺字形（子集范围），真机未见问题。

## 5. 已知限制 / 遗留

- MSVC 侧受 `candidateFonts()` 接口签名变更影响（`{path, 0}` 聚合初始化，C++20 标准），**建议下次 Windows 会话跑一次 0 错 0 警回归**（同 M8_ACCEPTANCE §6 遗留）。
- 无头冒烟（Xvfb 起 UI + 日志断言）已在探针 E2E 覆盖 ubuntu2204；其余容器为纯构建+ctest 格（GUI 冒烟可按需扩展进 run.sh）。
- AOSC OS：dpkg 系，`.deb` 直接兼容，留待 M10 打包阶段以格式兼容记录（无官方构建镜像）。
