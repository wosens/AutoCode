# -*- coding: utf-8 -*-
r"""AutoCode DSH 沙箱 mkdtemp 断继承修复（sitecustomize 注入层，2026-10-16）。

背景（pdfplumber「评估PDF转Word」DSH 助手七轮实录定案）：
  DSH windows-acl 沙箱的 workspace-write 强制 = 受限令牌 + 工作区/临时能力 SID 的
  NTFS DACL 授权（ACE 自工作区根继承）。CPython 的 tempfile.mkdtemp 内部走
  os.mkdir(path, 0o600)——显式 mode 在 Windows 上生成「保护型 owner-only DACL、
  不继承父 ACE」的目录，能力 SID 的授权 ACE 落空，受限令牌子进程（pip 等）在该
  目录内建目录/写文件全部 Errno 13（pip-unpack-*\*.whl.metadata 实害）。
  同进程背靠背对照：tempfile.mkdtemp 内 makedirs 被拒 / 普通 os.mkdir（默认
  mode，继承父 DACL）内一切正常——判别子即 mkdir 的显式 0o600。

修法（与现场手工 sitecustomize 同源，平台化到 AutoCode）：
  把 tempfile.mkdtemp 等价替换为普通 os.mkdir 实现（同 _sanitize_params 缺省、
  同唯一名重试环、同独占创建、同 nt PermissionError 分支）——默认继承 DACL，
  授权 ACE 天然在位，不触发任何审计/封禁路径。pip 安装逻辑零改动。

加载面：仅 DSH 会话——CDeepSeekRunner::EnsureProcess 把本目录（exe 同级
  dsh\\pyshim）前挂 PYTHONPATH；site.py 启动期导入本文件。隔离模式解释器
  （._pth，如嵌入式发行）忽略 PYTHONPATH，行为不变（无害）。

链式加载：本文件末尾移除自身目录后重导入 sitecustomize——用户环境若自有
  sitecustomize（如项目自装），其定制不因注入而丢失。
"""

import errno as _errno
import os as _os
import sys as _sys
import tempfile as _tempfile

_HERE = _os.path.dirname(_os.path.abspath(__file__))


def _mkdtemp_no_audit(suffix=None, prefix=None, dir=None):
    """Drop-in replacement for tempfile.mkdtemp without the sealed-DACL path."""
    prefix, suffix, dir, output_type = _tempfile._sanitize_params(prefix, suffix, dir)
    names = _tempfile._get_candidate_names()
    if output_type is bytes:
        names = map(_os.fsencode, names)
    for _seq in range(_tempfile.TMP_MAX):
        s_name = next(names)
        s_path = _os.path.join(dir, prefix + s_name + suffix)
        try:
            _os.mkdir(s_path)
        except FileExistsError:
            continue
        except PermissionError:
            if (_os.name == 'nt' and _os.path.isdir(dir) and
                    _os.access(dir, _os.W_OK)):
                continue
            raise
        return _os.path.abspath(s_path)
    raise FileExistsError(_errno.EEXIST,
                          "No usable temporary directory name found")


_tempfile.mkdtemp = _mkdtemp_no_audit

# 链式加载：让非本层的 sitecustomize（用户/项目自有）照常生效。不走 pop+import
#   （打断 site.py 正在进行的本模块导入，实测报 KeyError）；改为在剩余 sys.path
#   上定位下一个 sitecustomize.py、以独立模块名直接 exec。builtins 闩锁防别名
#   路径重入（同目录不同写法的 sys.path 项令本文件二次执行时不重复链式加载）。
import builtins as _builtins
if not getattr(_builtins, '_AC_PYSHIM_CHAINED', False):
    _builtins._AC_PYSHIM_CHAINED = True
    try:
        _rp = _os.path.realpath(_HERE)
        _sys.path[:] = [p for p in _sys.path
                        if p and _os.path.realpath(p) != _rp]
        import importlib.util as _ilu
        for _p in _sys.path:
            if not _p or _os.path.realpath(_p) == _rp:
                continue
            _cand = _os.path.join(_p, 'sitecustomize.py')
            if _os.path.isfile(_cand):
                _spec = _ilu.spec_from_file_location(
                    '_ac_next_sitecustomize', _cand)
                _mod = _ilu.module_from_spec(_spec)
                _sys.modules['_ac_next_sitecustomize'] = _mod
                _spec.loader.exec_module(_mod)
                break
    except Exception:
        pass
