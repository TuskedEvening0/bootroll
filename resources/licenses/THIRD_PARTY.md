# Third-Party Components & Licenses

| Component | Version | License | Source | Used as |
|---|---|---|---|---|
| Dear ImGui | master @ submodule | MIT | https://github.com/ocornut/imgui | static lib, bundled stb |
| tinygettext | v0.1.x @ submodule | Zlib | https://github.com/tinygettext/tinygettext | static lib (`TINYGETTEXT_UTF8_ONLY=ON`) |
| doctest | 2.5.0 (single header) | MIT | https://github.com/doctest/doctest | tests only (not shipped in bootroll.exe) |
| Noto Sans SC (subset, 4362 glyphs: ASCII + GB2312 L1 hanzi) | subset of NotoSansCJKsc-Regular | SIL OFL 1.1 | https://github.com/notofonts/noto-cjk | embedded fallback font (`resources/fonts/NotoSansSC-subset.otf`) |
| GRUB4DOS `grldr.mbr` | 0.4.6a 2020-08-09 (8192 B) | GPLv2 | https://github.com/chenall/grub4dos | embedded blob `resources/bootcode/grub4dos_mbr.bin` |
| GRUB4DOS `grldr` | 0.4.6a 2020-08-09 | GPLv2 | https://github.com/chenall/grub4dos | embedded blob `resources/bootcode/grub4dos_grldr.bin` (written to volume root) |
| GRUB4DOS `grldr.pbr` | 0.4.6a 2020-08-09 (11 sectors) | GPLv2 | https://github.com/chenall/grub4dos | embedded blob `resources/bootcode/grub4dos_pbr.bin` |
| WEE 63 `wee63.mbr` | shipped with grub4dos 0.4.6a | GPLv2 | https://github.com/chenall/grub4dos | embedded blob `resources/bootcode/wee63_mbr.bin` |
| Syslinux `mbr.bin` | 440 B prebuilt MBR | GPLv2 | https://www.syslinux.org | embedded blob `resources/bootcode/syslinux_mbr.bin` |

License texts: `OFL-1.1.txt` (font), `grub4dos-COPYING.txt`, `wee-COPYING.txt` (+
`wee-README.txt`), `syslinux-COPYING.txt`. imgui MIT and tinygettext Zlib texts live in
their submodule directories (`third_party/imgui/LICENSE.txt`, `third_party/tinygettext/LICENSE.md`).

Notes:
- GRUB4DOS / WEE / Syslinux are distributed here as **unmodified prebuilt binaries**
  taken from the upstream release archives listed above. GPLv2 §3 therefore applies:
  the corresponding source is available from those upstream repositories.
- Plop Boot Manager is NOT bundled: it is freeware but not open source and does not
  permit redistribution of its binaries (see `LEGAL.md`).
- No Microsoft boot code or any bytes extracted from BOOTICEx64.exe are embedded in this
  repository (see `LEGAL.md` for the full clean-room statement).
