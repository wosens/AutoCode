# -*- coding: utf-8 -*-
"""沙箱驱动 probe_hotnews.exe（自管进程路线）：stdout 落文件、窗口全隔离。"""
import subprocess, sys, time, os

SCRIPTS = r"C:\Users\Administrator\.claude\skills\ui-sandbox\scripts"
sys.path.insert(0, SCRIPTS)
import sandbox as sb  # noqa: E402

EXE_DIR = r"E:\wosens\Programs\VisualC++\QT\AutoCode\Execute"
EXE = os.path.join(EXE_DIR, "probe_hotnews.exe")
OUT = os.path.join(EXE_DIR, "probe_hotnews_out.txt")

s = sb.SandboxSession(name="hnprobe", mode="ghost")
outf = open(OUT, "wb")
proc = subprocess.Popen([EXE], cwd=EXE_DIR, stdout=outf, stderr=subprocess.STDOUT)
s.proc = proc
s.pid = proc.pid
s.exe = EXE
s._user_fg = sb.get_foreground_window()
s.start_auto_sweep()

deadline = time.time() + 300
seen = set()
isolated_any = False
while True:
    rc = proc.poll()
    if rc is not None:
        break
    if time.time() > deadline:
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], capture_output=True)
        print("[DRIVER] TIMEOUT 300s → killed")
        break
    try:
        for hwnd, pid, cls, title, visible in sb.enum_windows():
            if pid != proc.pid or not visible:
                continue
            if hwnd in seen:
                continue
            seen.add(hwnd)
            with s._sweep_lock:
                s.isolate(hwnd)
            isolated_any = True
            print(f"[DRIVER] isolated hwnd=0x{hwnd:X} cls={cls} title={title[:40]!r}")
    except Exception as e:
        print("[DRIVER] loop err:", e)
    time.sleep(0.2)

try:
    s.stop_auto_sweep()
except Exception:
    pass
try:
    s._save()
except Exception:
    pass
outf.close()
print(f"[DRIVER] exited rc={proc.returncode} isolated_any={isolated_any}")
print("---- probe output ----")
data = open(OUT, "rb").read()
try:
    print(data.decode("utf-8", errors="replace"))
except Exception as e:
    print("decode err", e)
