# Legal Information / 法律信息

bootroll — an open-source boot-sector & BCD maintenance utility with an ImGui UI.
本文件说明 bootroll 的法律定位、内嵌第三方组件及其许可义务。

## 1. Clean-room statement / 净室开发声明

bootroll is an **independent, from-scratch implementation** (original C++20 source).
BOOTICEx64.exe was used only as a **black-box behavioral reference**: its UI layout and
feature set were observed by running it, never by extracting code, resources, strings,
or any bytes from its binary. Nothing in this repository derives from BOOTICEx64.exe or
from Microsoft's boot code. The `BOOTICEx64.exe*` files that may exist in a working
directory are the developer's private reference artifacts and are **not part of the
project, not required to run bootroll, and must not be redistributed**.

bootroll 为**独立从零实现**（原创 C++20 代码）。BOOTICEx64.exe 仅作为**黑盒行为参考**：
只观察其界面布局与功能表现，不提取、不反汇编、不复制其中任何代码、资源或字节。
本仓库不包含任何源自 BOOTICEx64.exe 或 Microsoft 引导代码的内容。工作目录中可能存在的
`BOOTICEx64.exe*` 文件是开发者的私有参考物，**不属于本项目、运行 bootroll 无需它、
且不得随本项目再分发**。

## 2. Original code / 原创部分

All sources under `src/`, `tests/`, and `cmake/` are original work. The bundled
Windows NT 6.x-style MBR is bootroll's own 16-bit assembly, written from the publicly
documented on-disk behavior of the NT6 family MBR (original source:
`resources/bootcode/nt6_mbr.asm`); it contains no Microsoft code.

`src/`、`tests/`、`cmake/` 下所有代码均为原创。内置的 Windows NT 6.x 风格 MBR 是
bootroll 自行编写的 16 位汇编（源码 `resources/bootcode/nt6_mbr.asm`），依据 NT6 系列
MBR 的公开磁盘行为描述实现，不含任何微软代码。

## 3. Embedded third-party components / 内嵌第三方组件

Full inventory, versions, sources and license texts: **`resources/licenses/THIRD_PARTY.md`**
(and the license files next to it). Summary:

| Component | License | Form |
|---|---|---|
| Dear ImGui | MIT | statically linked |
| tinygettext | Zlib | statically linked |
| Noto Sans SC (subset) | SIL OFL 1.1 | embedded font |
| GRUB4DOS grldr.mbr / grldr / grldr.pbr | GPLv2 | unmodified prebuilt blobs |
| WEE 63 mbr | GPLv2 | unmodified prebuilt blob |
| Syslinux mbr.bin | GPLv2 | unmodified prebuilt blob |

GPLv2 obligations: the blobs are distributed **unmodified**, accompanied by their
license texts (`resources/licenses/`), with the corresponding source obtainable from the
upstream repositories listed in THIRD_PARTY.md (GPLv2 §3(c) source offer).

## 4. What is deliberately NOT bundled / 有意不内置的部分

- **Plop Boot Manager**: freeware but not open source; its redistribution terms do not
  allow rebundling. Plop 安装器闭源、不授权再分发，bootroll 不内置。
- **Windows NT partition boot sectors (PBR)**: never shipped as blobs. To install an
  NT-family PBR, use bootroll's "extract from a reference volume" feature, or supply
  your own sector file. NT 系列 PBR 不内置，请用"从参考卷提取"或自备文件。

## 5. Runtime requirements / 运行时

bootroll.exe is a single portable file: statically linked CRT (no VC++ Redistributable),
resources and translations embedded. It depends only on OS components of Windows 10+
(d3d11, d3dcompiler_47, dxgi, imm32, shell32, comdlg32, setupapi, ole32, advapi32,
dbghelp) — all shipped with the operating system.

bootroll.exe 为单文件便携程序：静态链接 CRT（无需 VC++ 运行库），字体与翻译全部内嵌，
仅依赖 Windows 10+ 自带系统组件。

## 6. Disclaimer / 免责声明

bootroll modifies boot sectors, partition tables and BCD hives. Even with the automatic
pre-write backup feature, **a failed write can make a disk or volume unbootable**. Use
at your own risk on test images/VHDs first; the authors accept no liability for data
loss. bootroll 会修改引导扇区、分区表和 BCD 配置单元。即使有写前自动备份，
**写入失败仍可能导致磁盘或分区无法引导**。请先在测试镜像/VHD 上验证；
对任何数据损失，作者不承担责任。

GRUB4DOS, WEE, Syslinux, Windows, BOOTICE and Plop are trademarks or product names of
their respective owners; this project is not affiliated with any of them.

GRUB4DOS、WEE、Syslinux、Windows、BOOTICE、Plop 等均为其各自所有者的商标或产品名称；
本项目与其无任何关联。
