cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0091 NEW) # MSVC_RUNTIME_LIBRARY abstraction

project(bootroll VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Statically linked CRT (no VC++ Redistributable dependency) - see CMakePresets.json too.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /MP)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    # GCC 12 has known -Wrestrict false positives on std::string concatenation
    # ("offsets [2, 9223372036854775807]" nonsense reports); fixed upstream in
    # GCC 13. Windows MSVC branch unaffected.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
       AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12.0
       AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13.0)
        add_compile_options(-Wno-restrict)
    endif()
endif()

# ---------------------------------------------------------------------------
# Dear ImGui (submodule): core + platform backends, built as a static lib.
#   Windows: win32 + dx11 backends.  Linux: glfw + opengl3 backends.
# ---------------------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
if(WIN32)
    set(IMGUI_BACKEND_SOURCES
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
else()
    set(IMGUI_BACKEND_SOURCES
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
endif()
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_BACKEND_SOURCES})
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_options(imgui PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W0>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>)

if(NOT WIN32)
    # Linux window + GL stack (decision: GLFW + OpenGL3, M8_PLAN §2).
    find_package(glfw3 REQUIRED)
    find_package(OpenGL REQUIRED)
endif()

# ---------------------------------------------------------------------------
# tinygettext (submodule): .po/.mo loading, UTF-8 only (no iconv dependency).
# ---------------------------------------------------------------------------
set(TINYGETTEXT_UTF8_ONLY ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/tinygettext)

# ---------------------------------------------------------------------------
# Tests (doctest). Disable with -DBOOTROLL_BUILD_TESTS=OFF.
# ---------------------------------------------------------------------------
option(BOOTROLL_BUILD_TESTS "Build unit tests" ON)
if(BOOTROLL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Embedded binaries (font + .po translations) generated into build/generated/.
# ---------------------------------------------------------------------------
include(cmake/EmbedBinary.cmake)

set(FONT_FILE ${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/NotoSansSC-subset.otf)
set(PO_ZH_FILE ${CMAKE_CURRENT_SOURCE_DIR}/resources/i18n/zh_CN.po)
set(PO_EN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/resources/i18n/en_US.po)
foreach(f ${FONT_FILE} ${PO_ZH_FILE} ${PO_EN_FILE})
    if(NOT EXISTS ${f})
        message(FATAL_ERROR "Missing resource file: ${f}")
    endif()
endforeach()

embed_binary(font_noto_sc  ${FONT_FILE} BOOTROLL_GEN_FONT)
embed_binary(po_zh_cn      ${PO_ZH_FILE} BOOTROLL_GEN_PO_ZH)
embed_binary(po_en_us      ${PO_EN_FILE} BOOTROLL_GEN_PO_EN)
# file(GENERATE) bakes file contents at configure time; without this the
# embedded font/translations go stale when the resources change.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${FONT_FILE} ${PO_ZH_FILE} ${PO_EN_FILE})

# License texts shown by the in-app license viewer (About tab) - the exe is a
# single portable file, so the texts must travel inside it.
set(LIC_GPL2_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/resources/licenses/grub4dos-COPYING.txt)
set(LIC_OFL_FILE   ${CMAKE_CURRENT_SOURCE_DIR}/resources/licenses/OFL-1.1.txt)
set(LIC_IMGUI_FILE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/LICENSE.txt)
set(LIC_TG_FILE    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinygettext/LICENSE.md)
foreach(f ${LIC_GPL2_FILE} ${LIC_OFL_FILE} ${LIC_IMGUI_FILE} ${LIC_TG_FILE})
    if(NOT EXISTS ${f})
        message(FATAL_ERROR "Missing license text: ${f}")
    endif()
endforeach()
embed_binary(lic_gpl2   ${LIC_GPL2_FILE}  BOOTROLL_GEN_LIC_GPL2)
embed_binary(lic_ofl    ${LIC_OFL_FILE}   BOOTROLL_GEN_LIC_OFL)
embed_binary(lic_imgui  ${LIC_IMGUI_FILE} BOOTROLL_GEN_LIC_IMGUI)
embed_binary(lic_tg     ${LIC_TG_FILE}    BOOTROLL_GEN_LIC_TG)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${LIC_GPL2_FILE} ${LIC_OFL_FILE} ${LIC_IMGUI_FILE} ${LIC_TG_FILE})

# bootroll's own NT6-style MBR (assembled from resources/bootcode/nt6_mbr.asm).
set(NT6_MBR_FILE ${CMAKE_CURRENT_SOURCE_DIR}/resources/bootcode/nt6_mbr.bin)
if(NOT EXISTS ${NT6_MBR_FILE})
    message(FATAL_ERROR "Missing boot code blob: ${NT6_MBR_FILE}")
endif()
embed_binary(bootcode_nt6_mbr ${NT6_MBR_FILE} BOOTROLL_GEN_NT6_MBR)

# Open-source boot code blobs (bundled binaries + licenses in
# resources/licenses/ - see THIRD_PARTY.md for versions and provenance).
set(BOOT_BLOBS
    bootcode_grub4dos_mbr:resources/bootcode/grub4dos_mbr.bin
    bootcode_grub4dos_pbr:resources/bootcode/grub4dos_pbr.bin
    bootcode_grub4dos_grldr:resources/bootcode/grub4dos_grldr.bin
    bootcode_wee63_mbr:resources/bootcode/wee63_mbr.bin
    bootcode_syslinux_mbr:resources/bootcode/syslinux_mbr.bin
)
foreach(ENTRY ${BOOT_BLOBS})
    string(REPLACE ":" ";" PAIR ${ENTRY})
    list(GET PAIR 0 TGT)
    list(GET PAIR 1 REL)
    set(BLOB_FILE ${CMAKE_CURRENT_SOURCE_DIR}/${REL})
    if(NOT EXISTS ${BLOB_FILE})
        message(FATAL_ERROR "Missing boot code blob: ${BLOB_FILE}")
    endif()
    embed_binary(${TGT} ${BLOB_FILE} BOOTROLL_GEN_BLOB)
    list(APPEND BOOTROLL_BLOB_SOURCES ${BOOTROLL_GEN_BLOB})
endforeach()

set(BOOTROLL_APP_VERSION ${PROJECT_VERSION})

# Git commit hash for the log header / About page ("unknown" outside a repo
# or before the first commit).
set(BOOTROLL_GIT_HASH "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    RESULT_VARIABLE BOOTROLL_GIT_RESULT
                    OUTPUT_VARIABLE BOOTROLL_GIT_HASH
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(NOT BOOTROLL_GIT_RESULT EQUAL 0)
        set(BOOTROLL_GIT_HASH "unknown")
    endif()
endif()

# ---------------------------------------------------------------------------
# bootroll executable (single portable exe, GUI subsystem)
# ---------------------------------------------------------------------------
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/app/Version.h.in
               ${CMAKE_BINARY_DIR}/generated/Version.h @ONLY)

# Per-platform sources. core/ui/app stay shared; only src/platform/<os> and
# the ImGui backend wiring differ (M8).
if(WIN32)
    set(BOOTROLL_PLATFORM_SOURCES
        src/platform/win/PlatformWin.h   src/platform/win/PlatformWin.cpp
        src/platform/win/UefiWin.h       src/platform/win/UefiWin.cpp
        src/platform/win/Win32Window.h   src/platform/win/Win32Window.cpp
        src/platform/win/RenderDX11.h    src/platform/win/RenderDX11.cpp
        src/platform/win/DiskWin.h       src/platform/win/DiskWin.cpp
        src/platform/win/VolumeWin.h     src/platform/win/VolumeWin.cpp
        src/platform/win/ElevateWin.h    src/platform/win/ElevateWin.cpp)
    set(BOOTROLL_EXECUTABLE_WIN32 WIN32)
else()
    set(BOOTROLL_PLATFORM_SOURCES
        src/platform/linux/PlatformLinux.h   src/platform/linux/PlatformLinux.cpp
        src/platform/linux/UefiVarsLinux.h   src/platform/linux/UefiVarsLinux.cpp
        src/platform/linux/GlfwWindow.h      src/platform/linux/GlfwWindow.cpp
        src/platform/linux/RenderGL.h        src/platform/linux/RenderGL.cpp
        src/platform/linux/DiskAccessLinux.h src/platform/linux/DiskAccessLinux.cpp
        src/platform/linux/VolumeLinux.h     src/platform/linux/VolumeLinux.cpp
        src/platform/linux/SysFs.h           src/platform/linux/SysFs.cpp)
    set(BOOTROLL_EXECUTABLE_WIN32 "") # console subsystem; GLFW owns the window
endif()

add_executable(bootroll ${BOOTROLL_EXECUTABLE_WIN32}
    src/main.cpp
    src/app/App.h              src/app/App.cpp
    src/app/CrashHandler.h     src/app/CrashHandler.cpp
    src/app/I18n.h             src/app/I18n.cpp
    src/app/Settings.h         src/app/Settings.cpp
    src/core/disk/DiskInfo.h   src/core/disk/DiskInfo.cpp
    src/core/disk/IDiskAccess.h
    src/core/disk/PartitionTable.h src/core/disk/PartitionTable.cpp
    src/core/bcd/Hive.h        src/core/bcd/Hive.cpp
    src/core/bcd/Utf16.h       src/core/bcd/Utf16.cpp
    src/core/bcd/BcdStore.h    src/core/bcd/BcdStore.cpp
    src/core/bcd/BcdElements.h src/core/bcd/BcdElements.cpp
    src/core/bootcode/BootCode.h src/core/bootcode/BootCode.cpp
    src/core/bootcode/PbrCode.h  src/core/bootcode/PbrCode.cpp
    src/core/util/HexText.h    src/core/util/HexText.cpp
    src/core/hexedit/HexEdit.h src/core/hexedit/HexEdit.cpp
    src/core/uefi/UefiVars.h   src/core/uefi/UefiVars.cpp
    src/core/uefi/LoaderPresets.h src/core/uefi/LoaderPresets.cpp
    src/core/fat/FatVolume.h   src/core/fat/FatVolume.cpp
    src/platform/IPlatform.h
    src/platform/IUefiVars.h
    ${BOOTROLL_PLATFORM_SOURCES}
    src/ui/MainScreen.h              src/ui/MainScreen.cpp
    src/ui/BcdEditorScreen.h         src/ui/BcdEditorScreen.cpp
    src/ui/BcdProfessionalScreen.h   src/ui/BcdProfessionalScreen.cpp
    src/ui/MbrScreen.h               src/ui/MbrScreen.cpp
    src/ui/PbrScreen.h               src/ui/PbrScreen.cpp
    src/ui/Grub4dosScreen.h          src/ui/Grub4dosScreen.cpp
    src/ui/PartitionScreen.h         src/ui/PartitionScreen.cpp
    src/ui/SectorScreen.h            src/ui/SectorScreen.cpp
    src/ui/UefiScreen.h              src/ui/UefiScreen.cpp
    src/ui/EspFileDialog.h           src/ui/EspFileDialog.cpp
    src/ui/ElevateHint.h             src/ui/ElevateHint.cpp
    src/ui/AboutScreen.h             src/ui/AboutScreen.cpp
    src/ui/widgets/DiskPicker.h      src/ui/widgets/DiskPicker.cpp
    src/ui/widgets/HexView.h         src/ui/widgets/HexView.cpp
    ${BOOTROLL_GEN_FONT} ${BOOTROLL_GEN_PO_ZH} ${BOOTROLL_GEN_PO_EN}
    ${BOOTROLL_GEN_LIC_GPL2} ${BOOTROLL_GEN_LIC_OFL}
    ${BOOTROLL_GEN_LIC_IMGUI} ${BOOTROLL_GEN_LIC_TG}
    ${BOOTROLL_GEN_NT6_MBR}
    ${BOOTROLL_BLOB_SOURCES}
    ${CMAKE_BINARY_DIR}/generated/Version.h)

target_include_directories(bootroll PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated)

if(WIN32)
    target_link_libraries(bootroll PRIVATE imgui tinygettext
        d3d11 dxgi d3dcompiler imm32 dwmapi setupapi shell32 comdlg32)
    target_compile_definitions(bootroll PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    # pthread: required for C++20 std::thread. GLFW target pulls X11/Wayland deps.
    find_package(Threads REQUIRED)
    target_link_libraries(bootroll PRIVATE imgui tinygettext glfw OpenGL::GL Threads::Threads)

    # FHS install layout (M10). Portable semantics are unaffected: po/license/
    # bootcode are embedded at configure time; the only runtime-external
    # dependency is system fonts (see PlatformLinux::candidateFonts).
    install(TARGETS bootroll RUNTIME DESTINATION bin)
    install(FILES resources/packaging/bootroll.desktop
            DESTINATION share/applications)
    install(FILES resources/packaging/icon/bootroll-128.png
            DESTINATION share/icons/hicolor/128x128/apps
            RENAME bootroll.png)
endif()

# Crash triage: -DBOOTROLL_ASAN=ON builds the exe with AddressSanitizer
# (run with ASAN_OPTIONS=log_path=<file> since a GUI app has no stderr).
option(BOOTROLL_ASAN "Build bootroll with AddressSanitizer" OFF)
if(BOOTROLL_ASAN)
    if(MSVC)
        target_compile_options(bootroll PRIVATE /fsanitize=address)
    else()
        target_compile_options(bootroll PRIVATE -fsanitize=address)
        target_link_options(bootroll PRIVATE -fsanitize=address)
    endif()
endif()

set_target_properties(bootroll PROPERTIES
    OUTPUT_NAME bootroll
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# Reserved for later milestones (per plan):
#   M1+: core/bcd (Hive, BcdStore, BcdElements, Templates) + tests/ (CTest+doctest)
#   M3+: core/bootcode (MbrCode, PbrCode, blobs) + core/disk/PartitionTable
#   M4 : ui/Grub4dosScreen, ui/PartitionScreen
#   M5 : ui/SectorScreen, ui/widgets/HexView
#   M7 : UEFI boot entry management (done)
#   M8 : platform/linux backend + linux-gcc preset activation
#   M9 : distro adaptation - gcc11 floor, build matrix, runtime font/dialog chains
#   M11: loader quick-add - systemd-boot / Limine (+rEFInd) ESP scan + Boot entry presets
# ---------------------------------------------------------------------------
