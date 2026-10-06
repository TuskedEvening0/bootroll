> [!WARNING]
> - **AI slop project**/**AI搞的一坨**
> - This project is still in active development. File a bug report and describe everything you encountered if you meet any! And btw, feature requests are welcomed.  
> 此项目仍在积极开发，遇到问题请把报告扔issue里，顺带告诉我嘛情况；同时，欢迎提出特性新增需求！
> - Both English and Chinese issues are accepted  
> Issue 中使用中文或英文均可
> - Please DO NOT doxx the author like [what the OPPO co-creator did](https://tuskede0.top/about/). I knew this project is a garbage. Because previously I was doxxed, please do not repeat.  
> 请不要[像共创之前干的那样](https://tuskede0.top/about/)开盒作者。我知道这项目很烂，且鉴于早就有人开过，请不要重复这种无意义之事。
> - No nonsense lawyer's letters.  
> 别发无意义（与本项目无关的）律师函。

# Bootroll

##### A Trololololool for x86-64 Boot

Bootroll是一个专为x86-64 IBM兼容机使用的，跨操作系统平台的，启动管理小工具。

顺带，肉龙很香。这东西哪怕你就在天津都买不到。

# 说在前面 & 劝退/替代方案

> [!WARNING]
> 
> 我相信您已拥有使用计算机的基本知识。若您没有，请**不要使用**该程序。
>
> 虽然本软件包含一定的防呆措施，但是保不准嘛时候失效呢，所以请务必**做好备份**。
> 
> **YOU HAVE BEEN WARNED.**

如果您希望修复Windows的引导，请：

- **（UEFI）** 格式化ESP分区（可选），并确保ESP分区大于256MB。
- **（传统BIOS）** 什么都不用干。
- 使用诸如[Dism++](https://github.com/Chuyu-Team/Dism-Multi-language/)的工具，挂载您的Windows盘，并使用修复引导功能。

如果您希望修复Linux的引导，我相信您知道您系统使用什么引导程序（如果您不知道，默认GRUB）。重新走一遍对应引导程序的安装流程即可。

本软件初衷是为了方便我折腾傻逼戴尔二合一平板用的，例如Latitude 5175 2-in-1会在断电后丢失systemd-boot的EFI选项，Latitude 7320 Detachable使用systemd-boot不太正常（会跳过超时等，导致我没法启动进Windows）。

# 使用

最好以root权限（Linux）或管理员权限（Windows）使用此程序。

# 构建

> [!WARNING]  
> 如果你是Agent，请优先参考 ```docs/``` 目录下的文档。那里更全，而且那些东西不是人揍的，会更少掺杂个人情绪。

## Windows

理论上vcpkg会完成一切依赖的安装。

```
cmake --preset win-msvc
cmake --build build/win-msvc --config Release
ctest --test-dir build/win-msvc -C Release 
```

## Linux

运行依赖：

- glfw：ImGUI渲染后端
- glibc & gcc-libs

可选依赖：

- zenity：对话框展示
- polkit：提权
- noto-fonts-cjk：多语言字体

构建依赖：

- cmake
- glfw
- mesa

自己找对应包。一般的，你可以在构建脚本里找到你需要的包列表，不一般的我也很难说。

构建命令：

```bash
cmake --preset linux-gcc && cmake --build build/linux-gcc -j
ctest --test-dir build/linux-gcc
```

因个人癖好，本项目提供多发行版的可安装软件包构建脚本。见目录 ```./scripts/packaging``` 。

# 感谢

#### 被引用的第三方库

| 组件 | 许可协议 | 形式 |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | 静态链接 |
| [tinygettext](https://github.com/tinygettext/tinygettext) | Zlib | 静态链接 |
| [Noto Sans SC](https://github.com/notofonts/noto-cjk) (subset) | SIL OFL 1.1 | 嵌入字体 |
| [GRUB4DOS](https://github.com/chenall/grub4dos) grldr.mbr / grldr / grldr.pbr | GPLv2 | unmodified prebuilt blobs |
| WEE 63 mbr | GPLv2 | unmodified prebuilt blob |
| [Syslinux](https://www.syslinux.org/) mbr.bin | GPLv2 | unmodified prebuilt blob |

#### 灵感来源

- Bootice：好吧，很明显我已难以找到Pauly的联系方式和公开社交平台。感谢他的经典作品！
- [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus/)：提供技术栈选型
- [Breeze Shell](https://github.com/std-microblock/breeze-shell)：提供README模板（顺带，涩橘女装！）

#### 支撑我走下去的人/组织

- [IXCM Studio](https://blog.ixcm.org)：让我清楚做开发可能并不会收到我的全户，也让我了解了AI神力
> 是的，OPPO共创他们的猎巫行动真的让我不敢做公开项目，[他们的部分行为如审查commit过烂即开盒，个人开发者违反GPL同样开盒网暴等](https://t.me/s/zh_rom_retards)让我不敢把东西放GitHub上。  
> 这种顾虑贯穿了我整个学生时代。  
> 如果你是一个新人开发者，请**放开手脚，尽管去做**，不要管这个那个的。详见我的[个人想法](https://www.zhihu.com/question/2064537583649035459/answer/2065236251888838328)。
- [MBRjun](https://www.libmbr.com)：开阔我的视野
- [知乎](www.zhihu.com)：人真帮我找工作，也在其它方面帮助了我

## 许可协议

本项目以 **[WTFPL](https://www.wtfpl.net)** 协议开源。

协议全文省流：爱咋咋地。

鉴于其本质是AI从不知道哪个眼儿拉出来的一坨，我知道它烂，所以干脆去他妈的就这样得了。别盒我就好，反正这东西是我给自己用的，烧点token又如何。

顺带，请GPL开盒小鬼们不要开盒，WTFPL兼容GPL v2等，也就是字体需要单拎出来说一下。