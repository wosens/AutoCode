#pragma once

#include <QString>

// AutoCode 三段式版本号：v主.次.编译号（如 v1.0.345）。
//   主/次版本与编译号都在构建期生成（build/gen/AutoCodeVersion.h，由
//   scripts/GenVersion.cmake 在 PRE_BUILD 阶段产出：主/次版本来自 CMakeLists.txt
//   的 project(VERSION)，编译号每次编译自动 +1）——源码内无版本数字，改版本只动 CMakeLists。
#include "AutoCodeVersion.h"

// 版本号字符串，形如 "v1.0.345"。
inline QString AutoCodeVersionText() {
    return QStringLiteral("v%1.%2.%3")
        .arg(AUTOCODE_VER_MAJOR)
        .arg(AUTOCODE_VER_MINOR)
        .arg(AUTOCODE_VER_BUILD);
}
