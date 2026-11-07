# CopyLockedTolerant.cmake —— copy_if_different 的「占用容忍」版（2026-11-05）。
#
# 背景（用户指令：编译时文件被占用按 ~/.claude/CLAUDE.md 规则处理）：部署
#   fileread.dll/code.exe 到 exe 目录时，运行中的 AutoCode 实例锁着同名文件，
#   cmake -E copy_if_different 直接失败并把整个构建拖崩。
#
# 规则（CLAUDE.md「Exe Update」）：运行中文件**可改名/移动、不可删除覆盖**——
#   旧件改名（<名>.old-<时间戳>）→ 移入同目录 ~temp → 再正常复制新件；
#   旧件待程序退出后由清理逻辑收走。
#
# 用法（POST_BUILD，单文件单次调用）：
#   COMMAND ${CMAKE_COMMAND} -DSRC=<源文件> -DDST=<目标目录>
#           -P <根>/scripts/CopyLockedTolerant.cmake
# 语义：
#   1) 源不存在 → WARNING（不破构建）；
#   2) 目标存在且 MD5 相同 → 跳过（同 copy_if_different）；
#   3) 不同 → 旧件收档到 <DST>/~temp/<名>.old-<时间戳>（改名对运行中文件
#      合法）→ 复制新件；
#   4) 收档/复制任一步失败 → 仅 WARNING 不破构建（旧件继续服役，等实例
#      退出后下次构建自动补上——MD5 比对天然重试）。
#
# 注：rename/copy 均走 execute_process（脚本内 file(RENAME)/file(COPY) 失败
#   是 FATAL_ERROR 会杀脚本破构建；execute_process 只回码，可控）。

if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "CopyLockedTolerant: -DSRC=<file> -DDST=<dir> required")
endif()

if(NOT EXISTS "${SRC}")
    message(WARNING "CopyLockedTolerant: source missing: ${SRC}")
    return()
endif()

get_filename_component(_name "${SRC}" NAME)
set(_dst "${DST}/${_name}")

if(EXISTS "${_dst}")
    file(MD5 "${SRC}" _m1)
    file(MD5 "${_dst}" _m2)
    if(_m1 STREQUAL _m2)
        return()   # 内容一致：与 copy_if_different 同口径，零动作
    endif()

    # 内容不同 → 需替换。目标可能被运行中实例锁定：按规则先收档旧件。
    string(TIMESTAMP _ts "%Y%m%d-%H%M%S")
    set(_stashDir "${DST}/~temp")
    file(MAKE_DIRECTORY "${_stashDir}")
    set(_stash "${_stashDir}/${_name}.old-${_ts}")
    set(_i 0)
    while(EXISTS "${_stash}")   # 同秒撞名（连编）→ 追加序号
        math(EXPR _i "${_i} + 1")
        set(_stash "${_stashDir}/${_name}.old-${_ts}-${_i}")
    endwhile()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E rename "${_dst}" "${_stash}"
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        # 改名也失败（罕见：句柄挂 deletion-pending 等）——直接试复制，
        # 成则旧映射退役、新文件顶替；败则走末尾 WARNING。
        message(STATUS "CopyLockedTolerant: rename failed (${_dst}), try direct copy")
    endif()
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy "${SRC}" "${_dst}"
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(WARNING
        "CopyLockedTolerant: copy failed (locked?): ${SRC} -> ${_dst}\n"
        "旧件保持服役；请退出占用它的实例后重新构建，MD5 比对会自动补上。")
endif()
