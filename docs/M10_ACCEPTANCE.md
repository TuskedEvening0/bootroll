# M10 验收记录（打包与发布）

> 日期：2026-10-06 · 工具链：scripts/packaging/*（全容器，ROADMAP 决策 #3）· 宿主 Arch
> 结论：**M10_PLAN §0 验收清单 8 项全部通过**。五类产物齐备（dist/ + SHA256SUMS），逐格式安装冒烟全绿；lintian/rpmlint 记录不门禁。图标为占位资产（遗留）。

## 1. 逐项验收结果

| # | 验收项 | 结果 | 证据 |
|---|---|---|---|
| 1 | deb（ubuntu:22.04） | ✅ | `dpkg-deb` 出包 2.0MB；容器 `apt install ./deb`（依赖自动解析）→ Xvfb 冒烟：日志 `boot 0.2.0 (commit 9aa6a8a)` + `disks enumerated: 2 in 46 ms` → `dpkg -r` 干净；desktop-file-validate ✓；lintian 报告存档（残留见 §4） |
| 2 | rpm（fedora + tumbleweed） | ✅ | fc44 + tumbleweed 双包（find-requires 自动库依赖，无跨发行版包名硬编码）；`dnf/zypper install` → Xvfb 冒烟（`boot 0.2.0` + 枚举行）→ 卸载干净；rpmlint 报告存档 |
| 3 | arch（archlinux:base-devel） | ✅ | makepkg（builder 用户，源码 tarball + SKIPINTEG）→ `pacman -U` → 冒烟：`boot 0.2.0` + `disks enumerated: 2 in 86 ms` → `pacman -R` 干净 |
| 4 | AppImage（ubuntu:22.04） | ✅ | linuxdeploy 打包（sha256 见 DISTRO_NOTES §2b）；容器内 `APPIMAGE_EXTRACT_AND_RUN=1` + Xvfb 进程存活冒烟 ✓。**注**：squashfs 只读 → 便携日志/ini 写入静默失败（预期；宿主 Wayland 冒烟由用户实测） |
| 5 | tar.gz | ✅ | `bootroll-0.2.0-linux-x86_64.tar.gz`（二进制 + LICENSE，glibc 2.35 地板） |
| 6 | 元数据合规 | ✅ | deb Depends 只写 libc6/libstdc++6/libgcc-s1/libglfw3/libgl1；rpm 库依赖 find-requires 自动生成；zenity/polkit → Recommends/Suggests；CJK 字体注释（包名未全实测，内嵌字体兜底）；版本单一来源（project VERSION → deb/rpm/PKGBUILD 自动注入）；x86_64 only（ExclusiveArch/arch） |
| 7 | 桌面项/图标 | ✅（占位图标） | desktop-file-validate ✓；图标为 ImageMagick 生成的占位（128px 深底 "B"），**正式版前待用户定稿替换** |
| 8 | 产物清单 + SHA256SUMS | ✅ | dist/ 六产物 + SHA256SUMS（5 项主产物）+ reports/{lintian,rpmlint}×3 |

## 2. 打包管线要点（scripts/package.sh + scripts/packaging/*）

- **staging 裁剪**：tinygettext 自带 install() 会把静态库/头文件/.pc 泄漏进 DESTDIR → deb/arch/appimage 三处 staging 统一 `rm -rf usr/lib usr/include`（rpm 靠 %files 白名单天然免疫）。
- **冒烟看门狗**：`Xvfb :31 &` + `timeout 8 <app> || rc=$?` —— exit 124 = 应用全程存活；**pkill/procps/xvfb-run 在精简容器里都不可依赖**（fedora 无 pkill、无 which——用 `command -v`）。`|| rc=$?` 是 set -e 下的保命写法（简单命令失败会静默退出，rc 捕获不到）。
- **rpm 双发行版**：BuildRequires 用 `%if 0%{?suse_version}` 分支（mesa-libGL-devel vs Mesa-libGL-devel）；smoke 按 os-release ID 选自家 rpm（跨发行版 Requires: glfw 会拒装——provides 名不同，这正是分容器出包的原因）。
- **openSUSE 本地 unsigned rpm**：`zypper --no-gpg-checks` 必须。
- **deb commit 哈希**：容器内 `/work` 属主跨界 → `git config --system --add safe.directory /work`，否则横幅 "commit unknown"。

## 3. 安装模式已知行为（记录）

- 便携语义：ini/log 落 exe 目录 → **deb/rpm/arch 安装后（/usr/bin）非 root 用户日志/设置静默失效**（容器 root 冒烟不受影响）。XDG 回退列为后续改进项（见 §5）。
- AppImage：只读挂载，同上；UI 功能不受影响。

## 4. lintian / rpmlint 残留（记录，不门禁）

- deb：`no-changelog`、`no-manual-page`、`copyright-without-copyright-notice`（LEGAL.md 非.debian copyright 格式）——正式发行化范畴。
- rpm：spelling-error（bootcode/imgui/tinygettext 专有名词）、`script-without-shebang`（%license 的 LEGAL.md）。

## 5. 遗留事项

- [ ] **图标定稿**（当前为占位资产，用户确认后替换 resources/packaging/icon/bootroll-128.png）。
- [ ] 安装模式 XDG 回退（日志/ini：root 安装目录不可写时切 XDG state/config 目录）。
- [ ] zenity/polkit/CJK 字体 Recommends 包名全量实测（rpm Suggests 未实测不阻断安装）。
- [ ] AppImage linuxdeploy 固定 release tag（现为 continuous，hash 已记录）。
- [ ] deb changelog/man page（正式发行化）。
- [ ] Windows 侧回归 + Windows 打包 → `docs/HANDOFF_WIN.md`（独立交接）。
