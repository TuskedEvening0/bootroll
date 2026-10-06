# Windows 端交接（M9/M10 之后）

> 2026-10-06，Linux 端 M9（发行版适配）+ M10（Linux 打包）完成后，整理给 Windows 端 Agent 的待办。Linux 侧共用代码的变更如下，**需要一次 MSVC 0 错 0 警回归 + ctest 确认**。

## 0. CI Windows job 现状

- `.github/workflows/ci.yml` 的 `windows-experimental` job **固定在 `windows-2022` runner**：windows-latest 已迁移到不含 VS2022 的镜像（VS 17 2022 generator 找不到实例，首跑已证实）。
- 若要升级到新镜像/VS2026：改 runner 标签 + preset 的 generator（`Visual Studio 18 2026`），并重新验证 `BOOTROLL_MSVC_RUNTIME_LIBRARY` 静态 /MT 设置。

## 1. 影响共用代码的变更（Windows 需回归的点）

| 变更 | 位置 | Windows 侧关注点 |
|---|---|---|
| `IPlatform::candidateFontPaths()` → `candidateFonts()`，返回 `std::vector<FontCandidate>{path, faceIndex}` | src/platform/IPlatform.h | `PlatformWin::candidateFonts()` 已同步实现（msyh.ttc 等，face 0）；**未在本机 MSVC 编译验证** |
| 新增 `IPlatform::elevateActionMsgId()/elevateDeclinedMsgId()` | src/platform/IPlatform.h | `PlatformWin` 已实现（"Restart as Administrator" 两条既有 msgid，po 未变） |
| 新增 `src/ui/ElevateHint.{h,cpp}`（非 root 横幅 + 一次性模态） | src/ui/ | Windows 分支文案走 `#ifdef _WIN32`（"Not running as administrator..."）；已加入 CMake 源列表 |
| `App::loadFonts` 使用 `ImFontConfig::FontNo` | src/app/App.cpp | Windows msyh.ttc face 0，行为不变 |
| `resources/packaging/bootroll.desktop` + 图标 | resources/packaging/ | Linux 专属，Windows 打包不受影响 |
| CMake `install()` 规则 | CMakeLists.txt（UNIX 分支内） | 仅 Linux 分支生效；Windows 打包（NSIS/便携 zip）留空待 Windows 端 |

## 2. 建议的回归步骤（Windows 端 Agent）

```powershell
cmake --preset win-msvc
cmake --build build/win-msvc --config Release   # 0 错 0 譣门禁
ctest --test-dir build/win-msvc -C Release      # 64 用例 / 538 断言
```

- 若 MSVC 对 `{utf8FromWide(p), 0}` 聚合初始化或 FontNo 有意见，按 MSVC 惯例修（不要动接口形状——Linux 端已按此定型）。
- UI 手测：非管理员启动 → 各特权页应显示黄色横幅 + "Restart as Administrator" 按钮 + 一次性模态（UAC 拒绝后出现红色 declined 行为）。

## 3. Windows 打包（M10 未覆盖，留待 Windows 端）

- ROADMAP 决策：M10 只做 Linux 五类产物；Windows 的安装器（NSIS/MSIX）或便携 zip 为独立工作项。
- 可复用：`scripts/package.sh` 的编排模式（staging → 打包 → 安装冒烟 → 卸载校验）可平移；产物落 `dist/` + SHA256SUMS 的约定保持一致。

## 4. 版本与发布线

- 当前 0.2.0（CMake project VERSION 单一来源，日志横幅已验证）。
- ROADMAP：正式版 = 1.0.0；发布动作 = bump 版本 + tag（Linux 产物管线对版本无假设，`scripts/package.sh` 自动读取）。
