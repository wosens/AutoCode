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

## UI 测试禁止物理输入（2026-10-30 实害立规）

- 测试 GUI 一律走内置沙箱/消息级输入。**禁止 SetCursorPos、mouse_event、
  SendInput、keybd_event、pyautogui 等一切真实鼠标键盘操控**——用户正在用
  这台电脑，真实输入会直接打断用户工作（FileServerClient 工程拖拽测试实测
  反复移动用户鼠标致无法正常办公）。
- 拖拽（TVN_BEGINDRAG / DragDetect 类）用 PostMessage 消息级链路即可触发；
  拖拽期间也不得把真实光标"停在受控落点"。若你断定目标程序只认硬件级输入，
  先输出证据请用户确认，不得擅自切换真实输入。
- 测试全程禁止挪动/最小化/置顶用户桌面上不属于被测程序的其他窗口来"腾地方"。
- **禁止用系统应用当测试夹具**（记事本/画图/计算器/浏览器/Office 等）：系统
  应用按设计不可被沙箱收纳，窗口会直接弹在用户桌面打断工作。需要"一个窗口"
  做测试（窗口枚举/截图/焦点验证等）时，用 sandbox launch 拉起工程内自建 exe
  ——幽灵化窗口仍是真实窗口（EnumWindows 可见、可截图可点按），测试语义不变
  且用户无感。
- **工程外副本（低完整性验证工作区等任何工程目录外的 exe）必须 sandbox
  launch 拉起**：Bash 直跑/cmd start 链断后沙箱认不出工程外进程，窗口会裸弹
  用户桌面（实测实害：验证助手在工程外工作区反复重启被测程序弹窗骚扰）。
  sandbox launch 对任意路径有效（主进程代启动+窗口弹出即自动收纳）。

## 网络排障速查
- git clone GitHub 报 "Failed to connect ... port 443"：用户有代理时用
  `git -c http.proxy=http://127.0.0.1:7897 -c https.proxy=http://127.0.0.1:7897 clone ...`
  单次代用；不要写全局 .gitconfig（代理关闭后 git 会全断）。
