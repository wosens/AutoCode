# AutoCode 版本头生成脚本（PRE_BUILD 每次编译自动执行）。
#   主/次版本：CMakeLists.txt 的 project(... VERSION x.y)（单一来源，经 -DVER_MAJOR/-DVER_MINOR 传入）。
#   编译号：build/autocode_build_number.txt 计数，每次编译 +1（删 build 目录会归 1）。
#   产物：${GEN_DIR}/AutoCodeVersion.h —— 内容变化才重写，避免无谓的全量重编。
# 传入参数（-D）：VER_MAJOR / VER_MINOR / STAMP_FILE / GEN_DIR

if(NOT DEFINED VER_MAJOR OR NOT DEFINED VER_MINOR
        OR NOT DEFINED STAMP_FILE OR NOT DEFINED GEN_DIR)
    message(FATAL_ERROR "GenVersion.cmake: VER_MAJOR/VER_MINOR/STAMP_FILE/GEN_DIR must be passed via -D")
endif()

set(HEADER_FILE "${GEN_DIR}/AutoCodeVersion.h")

if(EXISTS "${STAMP_FILE}")
    file(READ "${STAMP_FILE}" VER_BUILD)
    string(STRIP "${VER_BUILD}" VER_BUILD)
    if(NOT VER_BUILD MATCHES "^[0-9]+$")
        set(VER_BUILD 0)
    endif()
else()
    set(VER_BUILD 0)
endif()

math(EXPR VER_BUILD "${VER_BUILD} + 1")
file(WRITE "${STAMP_FILE}" "${VER_BUILD}")

set(HEADER_CONTENT
"// 本文件由 CMake 自动生成（scripts/GenVersion.cmake），勿手改。
// 主/次版本源自 CMakeLists.txt 的 project(VERSION)，编译号每次编译自动 +1。
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
