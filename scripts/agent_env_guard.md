# AutoCode 运行环境约定（助手必读）

## 配置文件是 GBK 编码 —— 禁止直接编辑
- exe 同目录 `config.ini` 按 Windows ANSI（中文系统=GBK）读写。
- **严禁**用 edit/write 工具直接改它：编辑器按 UTF-8 整文件重写会把中文值
  （如 LastProjectDir、HermesRoot 等含中文的路径）全部损坏成 U+FFFD 乱码，
  且无法恢复。
- 需要改配置时：让用户在 AutoCode 设置页操作；确需程序化修改，用 Python
  以 `encoding='gbk'` 读写且只替换目标行，禁止整文件重写。

## 长安装任务不要代跑
- `install_env.bat`（/dsh /hermes /pi /codex）、`pnpm install`、`pip install`
  等长任务常超 10 分钟且输出缓冲不刷新——宿主的停滞检测会直接终止进程。
- 正确做法：让用户回 AutoCode 设置页点「安装」按钮（独立管理员窗口执行、
  不受停滞检测影响），或指导用户自己开 cmd 运行：
  `cd /d <exe目录> && install_env.bat /hermes <仓库地址>`
- 安装脚本支持代理：先 `set AC_GIT_PROXY=http://127.0.0.1:7897`
  （git 走单次代理不污染全局 .gitconfig）、`set AC_PIP_PROXY=...`（pip）。
- 本沙箱会话可能禁止创建 junction/symlink（pnpm 依赖它）——在此会话里
  跑 pnpm 必败，请勿尝试，直接交给用户在正常窗口跑。

## 网络排障速查
- git clone GitHub 报 "Failed to connect ... port 443"：用户有代理时用
  `git -c http.proxy=http://127.0.0.1:7897 -c https.proxy=http://127.0.0.1:7897 clone ...`
  单次代用；不要写全局 .gitconfig（代理关闭后 git 会全断）。
