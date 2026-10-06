# M10 实施计划（打包与发布）

> 前置阅读：[ROADMAP.md](ROADMAP.md)（§0 决策 + §2 打包规划）→ [DISTRO_NOTES.md](DISTRO_NOTES.md)（包名对照：已实测/待实测）→ [M9_ACCEPTANCE.md](M9_ACCEPTANCE.md)。
> 里程碑定义（根 CMakeLists.txt 尾注）：`M10 : packaging - container-native deb/rpm/arch + AppImage/tar.gz + release pipeline`。
> 已定决策（ROADMAP §0）：**全容器打包**（含 Arch）；产物 = deb/rpm/pkg.tar.zst + tar.gz + AppImage；x86_64 only；AppImage 在 glibc 2.35 地板（ubuntu:22.04）上构建。

## 0. 目标与验收（先看这个）

**目标**：`scripts/package.sh` 一条命令，从干净的对应发行版容器产出全部五类安装/便携产物，并逐格式完成安装冒烟。

**验收清单（全部满足才算 M10 完成）**：

1. **deb**（ubuntu:22.04 容器 dpkg-deb）：`apt install ./bootroll_*.deb` 成功（依赖自动解析）→ Xvfb 冒烟（日志含版本横幅 + 枚举行）→ `dpkg -r` 干净；lintian 报告留档（不设硬门禁，记录）；
2. **rpm**（fedora + opensuse/tumbleweed 容器 rpmbuild）：`dnf/zypper install` 成功 → Xvfb 冒烟 → 卸载干净；rpmlint 报告留档；
3. **arch**（archlinux 容器 makepkg）：`pacman -U` 成功 → Xvfb 冒烟 → `pacman -R` 干净；
4. **AppImage**（ubuntu:22.04 容器 linuxdeploy）：`APPIMAGE_EXTRACT_AND_RUN=1` 容器内可启动（Xvfb 冒烟同上）；宿主 Wayland 冒烟由用户实测（无需安装/免 FUSE 双路径）；
5. **tar.gz**：解压后单文件可跑（对照 M8 便携语义）+ LICENSE 齐全；
6. **元数据合规**：硬依赖只写 glibc/libstdc++/glfw/libgl 一线（shlibs 自动解析优先）；zenity/polkit/CJK 字体进 Recommends/optdepends/Suggests；版本号单一来源（CMake project VERSION）；x86_64 only；
7. 图标/桌面项：`.desktop` 过 `desktop-file-validate`；**图标为占位资产（待用户定稿替换，M10_ACCEPTANCE 遗留）**；
8. 产物清单 + SHA256SUMS + `docs/DISTRO_NOTES.md` 更新（打包实测包名回填）。

## 1. 步骤一：安装布局（CMake install + 桌面集成）

- `install(TARGETS bootroll RUNTIME DESTINATION bin)`（仅 UNIX 分支；便携语义不受影响——po/许可/引导代码内嵌，运行时外部依赖仅字体）；
- `.desktop`（`resources/packaging/bootroll.desktop`）→ `share/applications/`；icon 128px → `share/icons/hicolor/128x128/apps/`；
- `desktop-file-validate` 在 deb 冒烟容器内执行。

## 2. 步骤二：五类产物管线（scripts/packaging/ + scripts/package.sh）

| 产物 | 容器 | 工具 | 备注 |
|---|---|---|---|
| deb | ubuntu:22.04 | dpkg-deb | staging 布局 usr/bin + usr/share；shlibs 依赖手写（对齐 M9 实测包名），Recommends 标注未实测项 |
| rpm | fedora / opensuse/tumbleweed | rpmbuild | 库依赖交给 rpm 自动 find-requires（provides 级）；spec 不写死小版本 |
| pkg.tar.zst | archlinux:base-devel | makepkg | build() 内跑 cmake（复用矩阵路径）；makedepends 对齐 Arch 包名 |
| AppImage | ubuntu:22.04 | linuxdeploy（下载 pin） | AppDir = usr/{bin,share/applications,share/icons}；容器内 `APPIMAGE_EXTRACT_AND_RUN=1` |
| tar.gz | 任一容器 | tar | bootroll + LICENSE 汇总 + README |

- orchestrator：`scripts/package.sh [deb|rpm|arch|appimage|targz|all]`，产物落 `dist/`（.gitignore），自动生成 SHA256SUMS。

## 3. 步骤三：逐格式安装冒烟（M10_PLAN §0 第 1–5 条）

- 复用 M9 的容器思路：装 → `xvfb-run` 起 → grep 日志（`boot 0.2.0`、`disks enumerated`）→ 卸载 → 残留检查；
- lintian（deb）/ rpmlint（fedora）报告存 `dist/reports/`（记录，不硬门禁）；
- openSUSE 的 rpm 用同一 spec 分容器构建（%dist 差异接受）。

## 4. 步骤四：发布件与文档

- `docs/M10_ACCEPTANCE.md`（逐项 + 证据）；`docs/HANDOFF_WIN.md`（Windows 侧交接：MSVC 回归清单 + Windows 打包留空）；HANDOFF/ROADMAP 状态更新；DISTRO_NOTES 包名回填；
- 版本策略：M10 开发期保持 0.2.0；**发布时**由用户确认 bump（1.0.0-rc1 → 1.0.0）+ 打 tag，管线对版本无假设（自动读取 project VERSION）。

## 5. 提交粒度

1. 安装布局 + 桌面项 + 图标占位 → 提交；
2. deb 管线 + 冒烟 → 提交；
3. rpm（双容器）+ arch + 冒烟 → 提交；
4. AppImage + tar.gz + SHA256SUMS → 提交；
5. 文档（acceptance / HANDOFF_WIN / 状态）→ 提交。

## 6. 风险登记

| 风险 | 缓解 |
|---|---|
| linuxdeploy/AppImage runtime 下载不可复现 | 固定 release URL + 容器内 sha256 校验（首次下载后把 hash 写入脚本） |
| Recommends 包名未实测（M9 遗留） | RPM 系用 Suggests（不存在也不阻断安装）；deb 用 Recommends（未知包仅 lintian 警告）；M10 内回填实测 |
| 许可混合（GRUB4DOS GPL-2.0 blob 内嵌） | 包 Description 标注许可构成；上架发行版仓库的许可审查留待发布期 |
| PKGBUILD 打包本地未发布源码 | makepkg `--skipinteg`（本地开发管线），正式发布切 git source |
| 幽灵工具通道（M8 §3 教训） | 写→读→git diff 核验；不可挽回风险（真盘/root 误操作）即停即报（用户指示） |
