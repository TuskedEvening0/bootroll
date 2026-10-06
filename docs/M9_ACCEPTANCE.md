# M9 验收记录（发行版适配与可移植性）

> 日期：2026-10-06 · 环境：宿主 Arch (gcc 16.2.1 / clang 23.1.1 / cmake 4.4.4) + Docker 容器矩阵
> 结论：**M9_PLAN §0 验收清单 6 项全部通过并留证**。编译器下限 gcc 11（Ubuntu 22.04）实测无代码阻碍；矩阵 8 格全绿；字体三层解析 + fail-closed 探针全过。

## 1. 逐项验收结果

| # | 验收项 | 结果 | 证据 |
|---|---|---|---|
| 1 | 容器构建矩阵全绿 | ✅ 8 格（含 clang14 + 主机上限回归） | 见 DISTRO_NOTES.md §1：ubuntu22.04 gcc11.4/clang14、debian12 gcc12.2、ubuntu24.04 gcc13.3、debian13 gcc14.2、fedora gcc16.2、tumbleweed gcc16.2、Arch gcc16.2+clang23——全部 0 错 0 警 + 64 用例/538 断言 |
| 2 | 无 CJK 字体：内嵌回退兜底 | ✅ E2E 实证 | 裸容器 Xvfb 跑真应用：日志 `font: <embedded NotoSansSC subset>`；防豆腐块回归：fontconfig+DejaVu（0 zh 覆盖）→ 候选为空（DISTRO_NOTES §4 场景 B/E2E） |
| 3 | 无 zenity / 无 polkit：fail-closed | ✅ 探针实证 | 裸容器 `confirmDialog=false`、`restartElevated=false`（PROBE_RESULT bad=0，DISTRO_NOTES §4 场景 A） |
| 4 | 字体候选链补全（实测为准） | ✅ 动态解析 | 弃"凭记忆硬编码"路线：`fc-list :lang=zh-cn` 覆盖校验 + `fc-match %{file}\|%{index}` 动态取面（SC face 2），静态表降级为兜底；Debian 布局（opentype/noto）动态命中实证 |
| 5 | 《发行版适配记录》 | ✅ | `docs/DISTRO_NOTES.md`（矩阵版本表、已实测/待 M10 包名对照、下限修复三件、字体解析四场景、已知限制） |
| 6 | po 同步 | ✅ 无新增文案 | M9 未新增 UI 字符串（纯平台/构建层改动），zh_CN.po 无变化 |

## 2. 下限审计结论（M9_PLAN §1）

- **gcc 11.4 + clang 14.0（Ubuntu 22.04）首跑即绿**：`std::format` 确认未用（日志为 snprintf 风格）；C++20 子集够用，无需特性门控。
- cmake 3.22（< presets 3.25）：矩阵/容器统一显式 `cmake -S -B`，根 CMakeLists `cmake_minimum_required(3.21)` 兼容；presets 保持本机开发用途。
- 真实问题两件均来自"次新编译器"而非下限：gcc 12 缺 `<cstddef>` 传递包含（HexEdit.h，已修）+ `-Wrestrict` 假阳性（版本门控 `-Wno-restrict`）；gcc 14 一次 `-Wfree-nonheap-object` 抖动未复现，留观察。

## 3. 探针记录（tests/tools/m9probe.cpp）

| 场景 | 环境 | 结果 |
|---|---|---|
| A | 无 fontconfig/字体/zenity/pkexec | candidateFonts 空；confirmDialog/restartElevated 均 false（fail-closed） |
| E2E | A + Xvfb + Mesa（无字体） | 应用正常起，`font: <embedded NotoSansSC subset>` |
| B | fontconfig + DejaVu（0 zh 覆盖） | candidateFonts 空（fc-match 模糊结果被覆盖校验拒绝） |
| D | fonts-noto-cjk | `NotoSansCJK-Regular.ttc [face 2]`（Debian 布局） |

四场景 `PROBE_RESULT bad=0`。

## 4. 实现偏差（与 M9_PLAN 的差异）

1. 字体链补全从"静态路径扩列"升级为"动态解析 + 覆盖校验"（fc-list/fc-match），静态表仅兜底——比计划更稳，且顺带修复了 M8 遗留的 JP 字形问题（f76268a）。
2. 矩阵未做 digest 级镜像 pin（tag pin）；cmake/编译器版本已按格记录于 DISTRO_NOTES，digest pin 留 M10 打包前加固。
3. 提交粒度：M9_PLAN §5 建议逐步提交，实际 3 次（矩阵+下限修复 / 字体+探针 / 文档+版本）。

## 5. 遗留事项（交接清单）

- [ ] MSVC 回归（Windows 会话）：`candidateFonts()` 接口签名变更 + `{path, 0}` 聚合初始化需 0 错 0 警验证（同 M8_ACCEPTANCE §6 遗留合并处理）。
- [ ] 矩阵镜像 digest pin（M10 打包前）。
- [ ] run.sh 增打 GLFW 运行库版本（debian12/2404/debian13 的 GLFW 小版本未记录）。
- [ ] `zenity`/`polkit`/CJK 字体/xvfb 的 RPM 系与 Arch 包名实测（M10 包元数据输入，DISTRO_NOTES §2 已标）。
