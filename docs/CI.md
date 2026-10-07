# CI / GitHub Actions

> 2026-10-06 接入。两个 workflow，均对齐本地管线：M9 构建矩阵（scripts/matrix）与 M10 打包（scripts/package.sh）。

## Workflows

### ci.yml — 构建 + 测试矩阵（push main / PR / 手动）

| Job | 说明 |
|---|---|
| `linux-matrix` × 8 | GitHub `container:` 跑与本地矩阵**同镜像同包名**的环境：ubuntu22.04 (gcc11 + clang14)、debian12 (gcc12)、ubuntu24.04 (gcc13)、debian13 (gcc14)、fedora、tumbleweed、arch——0 错 0 警门禁 + ctest 64/538 门禁 |
| `windows-experimental` | windows-latest + `win-msvc` preset 构建 + ctest；**continue-on-error**（MSVC 侧未经 Windows Agent 验证，见 HANDOFF_WIN.md，失败可见不阻断） |

注意：
- 容器 job 里 `actions/checkout` 需要 git → **provision 步骤必须在 checkout 之前**（每格先装包再取源码，submodules: recursive）。
- 包名清单与 `scripts/matrix/Dockerfile.*` 保持同步——改依赖时两处一起改（DISTRO_NOTES §1/§2 是对账表）。

### package.yml — 打包 + 发布（tag v* / 手动）

1. 版本守卫：tag `vX.Y.Z` 必须等于 CMake project VERSION（版本单一来源，不一致即失败）；
2. `scripts/package.sh all`（ubuntu-latest 自带 docker，跑六个打包容器）→ dist/ 五类产物 + SHA256SUMS；
3. 产物作为 workflow artifact 附着；
4. **tag 推送时**额外创建 GitHub Release（softprops/action-gh-release，generate_release_notes）。

## 本地等价命令

```bash
./scripts/build-matrix.sh          # = ci.yml linux-matrix
./scripts/package.sh all           # = package.yml 的构建步
```

## 发布流程（1.0.0 起）

```bash
# 1) bump CMakeLists project VERSION（如 1.0.0）
# 2) 提交并打 tag（必须与 VERSION 一致，package.yml 有守卫）
git commit -am "release: 1.0.0" && git tag v1.0.0 && git push origin main --tags
# 3) package.yml 自动出全套产物并挂到 GitHub Release
```

tag 重打（Release 尚未挂出前无副作用）：`git push origin :refs/tags/vX.Y.Z` 删远端 → 本地重 tag → 再推。已失败但不产生 Release 的 tag 可安全删除重打（2026-10-07 v1.0/v1.0.0 首跑即此情形：先版本守卫失败，后执行位 126，均无 Release 残留）。

## 已知限制

- Windows job 未经验证（HANDOFF_WIN.md），首跑可能红——属预期，由 Windows Agent 认领修复后摘掉 continue-on-error。
- 镜像按 tag pin（非 digest）；打包前加固项（M10_ACCEPTANCE §5）。
- AOSC 无 CI 覆盖（dpkg 系格式兼容，发布期人工验证）。
