# AutoCode 版本头生成脚本（PRE_BUILD 每次编译自动执行）。
# 2026.08.24 起按 %USERPROFILE%\.claude\CLAUDE.md「Version Number」规则实现：
#   计数依据 = 工程根 readme.txt 修改记录条数（行首 '[' 的行，每条一行）。
#   三段式 v主.次.编译号：
#     编译号 = 记录累计条数（持续累计、永不归零）；
#     次版本 = (条数 % 100) / 20 —— 每满 20 条 +1；
#     主版本 = 1 + 条数/100 —— 每满 100 条 +1 且次版本清零。
#   示例：v1.0.19 →（第 20 条）v1.1.20 →（第 100 条）v2.0.100 →（第 120 条）v2.1.120。
#   readme.txt 首行须维护「版本：vX.Y.N」与之一致（追加 Change Log 时同步检查更新）。
# 产物：${GEN_DIR}/AutoCodeVersion.h —— 内容变化才重写，避免无谓的全量重编。
# 传入参数（-D）：README_FILE / GEN_DIR
#
# ⚠ 计数实现说明（2026.08.24 踩坑记录，勿回退）：
#   CMake 文本模式 file(READ)/file(STRINGS) 对本文件 UTF-8 中文内容会错误切分
#   （多字节序列被当成分隔符，39 条只数出 5）。改用 file(READ ... HEX) 十六进制
#   字节串统计模式 "0a5b"（LF+'['）：UTF-8 多字节序列的每个字节都 ≥0x80，
#   绝不含 0x0A，故该计数对任意中文/全角内容安全且精确。

if(NOT DEFINED README_FILE OR NOT DEFINED GEN_DIR)
    message(FATAL_ERROR "GenVersion.cmake: README_FILE/GEN_DIR must be passed via -D")
endif()

if(NOT EXISTS "${README_FILE}")
    message(FATAL_ERROR "GenVersion.cmake: readme.txt not found: ${README_FILE}")
endif()

cmake_policy(SET CMP0007 NEW)

file(READ "${README_FILE}" README_HEX HEX)
string(REGEX MATCHALL "0a5b" ENTRY_MATCHES "${README_HEX}")
list(LENGTH ENTRY_MATCHES VER_BUILD)
if(VER_BUILD LESS 1)
    set(VER_BUILD 1)
endif()

# CLAUDE.md 递增规则：每满 100 条主版本 +1（次版本清零）；每满 20 条次版本 +1。
math(EXPR VER_MAJOR "1 + ${VER_BUILD} / 100")
math(EXPR VER_MINOR "(${VER_BUILD} % 100) / 20")

set(HEADER_FILE "${GEN_DIR}/AutoCodeVersion.h")
set(HEADER_CONTENT
"// 本文件由 CMake 自动生成（scripts/GenVersion.cmake），勿手改。
// 版本号按 CLAUDE.md 规则由 readme.txt 修改记录条数推导：v主.次.编译号
//   编译号=记录条数；每满 20 条次版本 +1；每满 100 条主版本 +1（次版本清零）。
#pragma once
#define AUTOCODE_VER_MAJOR ${VER_MAJOR}
#define AUTOCODE_VER_MINOR ${VER_MINOR}
#define AUTOCODE_VER_BUILD ${VER_BUILD}
")

if(EXISTS "${HEADER_FILE}")
    file(READ "${HEADER_FILE}" OLD_CONTENT)
else()
    set(OLD_CONTENT "")
endif()

if(NOT OLD_CONTENT STREQUAL HEADER_CONTENT)
    file(MAKE_DIRECTORY "${GEN_DIR}")
    file(WRITE "${HEADER_FILE}" "${HEADER_CONTENT}")
endif()
