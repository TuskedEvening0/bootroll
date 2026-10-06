# M9 实施计划（发行版适配与可移植性）

> 前置阅读：[ROADMAP.md](ROADMAP.md)（M9–M11 总览与已定决策）→ HANDOFF.md → ARCHITECTURE.md → PLATFORM_SEAMS.md → LESSONS.md。
> 里程碑定义（根 CMakeLists.txt 尾注，开工时追加）：`M9 : distro adaptation — gcc11 floor, build matrix, runtime font/dialog chains`。
> 决策记录（2026-10-05/06 与用户确认，详见 ROADMAP.md §0）：三段式；编译器下限 **gcc 11**（Ubuntu 22.04）；打包全容器化；产物三原生包 + tar.gz + AppImage。
> **启动前置**：用户当前已知问题修复完毕后再开工（2026-10-06 指示：暂不推进）。

## 0. 目标与验收（先看这个）

**目标**：同一份 core/ui/app/platform 代码，在 Debian 系 / RPM 系 / Arch 系容器矩阵里全部 0 错 0 警 + 测试全过；运行时差异（字体、对话框、提权）适配完成并有实测记录。

**验收清单（全部满足才算 M9 完成）**：

1. 容器构建矩阵全绿（每格 configure → build **0 错 0 警** → ctest 全过，64 用例 / 538 断言，除非期间有测试增删）：ubuntu:22.04（gcc 11 + clang 14）、debian:12（gcc 12）、ubuntu:24.04、debian:13、fedora:latest、opensuse/tumbleweed、Arch 本机基线回归（gcc 16 / clang 23）；
2. 无 CJK 字体的精简容器：内嵌字体回退兜底生效（日志 `font:` 行可证），中文 UI 可渲染非豆腐块；
3. 无 zenity / 无 polkit 容器：写操作 fail-closed（确认框缺失 → 拒绝写而非跳过），提权失败路径有明确错误提示；
4. 字体候选链补全并实测命中（Debian/Ubuntu/Fedora/openSUSE 路径以容器内 `fc-list` 实测为准）；
5. 《发行版适配记录》入 docs：各系编译器版本 / 运行时依赖 / 包名对照（zenity、polkit、glfw、字体包）+ 实测证据，兼作 M10 包元数据输入；
6. 新增 UI 文案同步 zh_CN.po / en_US.po（如无新文案，记录"无"）。

## 1. 步骤一：gcc 11 / clang 14 下限审计（Ubuntu 22.04 容器试编译）

- 22.04 系统 cmake 3.22 < CMakePresets 所需 3.25 → 容器内用显式 `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`（根 CMakeLists `cmake_minimum_required(3.21)` 兼容，已核实）；**不改 presets**。
- 重点排查（gcc 11/12 与 clang 14 的 C++20 缺口）：
  - `std::format`（gcc 13 起才有）——现日志代码为 `snprintf` 风格，预期未用，确认即可；
  - `<cstdint>` 等传递包含缺失（新 gcc 收紧，老 gcc 反而宽松，反向问题一般不出现；clang 14 对 incomplete type / 两阶段查找更严）；
  - concepts / ranges / 设计化初始化器顺序等细节；
- 发现即修，**修完必须回本机复验 gcc 16 + clang 23 仍 0 警**（上限不回退）；
- 记录：`CMAKE_CXX_STANDARD 20` 在 gcc 11 下实际可用的子集结论，写入适配记录（供 M10/M11 编码参考，避免引入 22.04 编不过的写法）。

## 2. 步骤二：构建矩阵脚本化

- `scripts/build-matrix.sh`（或逐 Dockerfile + 驱动脚本）：每容器 configure → build → ctest，构建输出 `grep -ciE "error|warning"` 必须为 0；
- 容器镜像 pin（tag 或 digest），保证可复现；构建缓存目录挂载宿主以加速重跑；
- 本机 Arch 基线作为对照列（M8 已绿，回归即可）；
- 产出：矩阵结果表（容器 × 编译器 × 结果 + 日志存档路径）。

## 3. 步骤三：运行时差异适配

- **字体链**：`PlatformLinux::candidateFontPaths` 补全各发行版 Noto CJK 路径——先在各容器 `fc-list | grep -i cjk` 实测真实路径再写候选链，不凭记忆；WenQuanYi / DroidSansFallback 候选保留；
- **内嵌字体兜底**（精简容器，无任何系统字体）：确认内嵌字体是否含 CJK 字形；不含则提出方案（换内嵌字体 / 包 Recommends CJK 字体 / 两者结合），**与用户确认体积权衡后实施**；
- **zenity 缺失**：confirmDialog fail-closed 路径复验（红线：确认框不可用 = 拒绝写操作并提示，绝不能静默放行）；
- **polkit 缺失**：`pkexec` 失败路径的用户提示复验；
- 各系包名对照表：zenity、polkit、glfw 运行时包、CJK 字体包（M10 直接引用）。

## 4. 步骤四：无头冒烟 + 文档

- 代表性容器（debian:12、ubuntu:22.04、fedora:latest、Arch）Xvfb :99 无头跑真流程，断言日志（字体命中行、`disks enumerated` 收敛、提权按钮在位），截图留证；
- 《发行版适配记录》写入 docs（独立文件 `DISTRO_NOTES.md` 或并入 ROADMAP 附录，动工时定）；
- 收尾：CMakeLists 尾注追加 M9 定义、HANDOFF.md 里程碑状态表更新、写 `M9_ACCEPTANCE.md`（沿用 M8 格式）、发 0.2.0。

## 5. 实施顺序与提交粒度

1. 下限审计 + 修复 → 提交（本机双基线 0 错 0 警不回退）；
2. 矩阵脚本 + 逐容器跑绿 → 每修通一个发行版一提交；
3. 字体链 / fail-closed 适配 → 提交；
4. 冒烟证据 + 文档 → 提交。

每步保持：本机 Arch gcc/clang 0 错 0 警 + 64/538 全过；工具通道纪律沿用（写→读→git diff 核验，见 M8_ACCEPTANCE §4）。
