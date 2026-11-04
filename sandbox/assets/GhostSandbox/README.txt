GhostSandbox 私有目录
====================

本目录存放「内置幽灵沙箱」动态库（GhostSandbox.dll）自有依赖与配置：

  - 依赖 DLL：如有随库分发的外部依赖，放本目录并在 sbx_start 内
    LoadLibraryExW(LOAD_WITH_ALTERED_SEARCH_PATH) 按本目录解析；
  - 私有配置：仅本沙箱认的参数文件（JSON/INI 均可），由库自读自写，
    宿主不感知（宿主只下发 [Sandbox] 通用键）；
  - 运行期产物请勿放这里（录像/截图统一在 exe\sandbox\rec|shot）。

文件说明：
  - manifest.json：沙箱清单（id/ABI 版本/能力声明），供人工核对与
    文档同步；宿主运行期只认 dll 导出符号，不读本文件。

注意：目录会被构建同步整目录覆盖（copy_directory），用户自加文件
在下次构建时保留（只增不删），但请勿在本目录手工编辑正本——
正本在源码树 sandbox/assets/GhostSandbox/。
