# DevStats 麒麟 ARM64 免管理员安装包

适用目标：银河麒麟桌面 V10 SP1 / 2303、ARM64（aarch64），glibc 2.31 或更新。
这是包含私有动态运行库的用户目录安装包；启动 DevStats 不需要安装 Qt、SQLite、
开发包，不执行 apt、sudo，不修改系统 Qt 或系统目录。它仍然依赖 Linux / glibc、
图形会话、系统字体以及与目标显卡匹配的驱动，不能视为全静态或零系统依赖程序。

English: [README-user.en.md](README-user.en.md).

## 解压即用

```sh
tar -xzf devstats-1.0.0-linux-arm64-user.tar.gz
cd devstats-1.0.0-linux-arm64-user
./devstats --show
```

命令行采样无需桌面：`./devstats --snapshot`。将整个解压目录放在当前用户拥有的
位置，保持目录完整。启动器自动定位自己的目录，可移动整个目录；不要直接执行
`libexec/devstats`。不要 source 启动脚本，也不需要把私有库写进 `.bashrc` 或系统环境。

## 安装到当前用户

```sh
./install-user.sh
```

默认安装目录为 `~/.local/opt/devstats-1.0.0`；命令入口为
`~/.local/bin/devstats`；菜单入口和图标位于
`${XDG_DATA_HOME:-~/.local/share}/applications` 和 `icons/hicolor/scalable/apps`。
安装器拒绝 root，也不会替你开启登录启动。默认数据位于当前用户的 XDG 数据目录，
配置位于 XDG 配置目录。可在应用设置中自行开启登录启动。

同一版本、同一内容重复安装安全。新版本使用不同的版本目录并切换本应用的入口，
保留旧版本。已经开启的登录启动会一并改为启动新版本，否则下次登录启动的还是旧版本。
升级前请先在托盘菜单退出 DevStats：旧版本还在运行时，再打开看到的仍是旧版本，安装器会提示。
已有的非 DevStats 文件、目录、命令或菜单入口会导致安装中止。
同版本内容不同或已修改时，使用新的空目录，不覆盖原文件。自定义位置示例：

```sh
DEVSTATS_USER_INSTALL_DIR="$HOME/应用/DevStats 1.0.0" \
DEVSTATS_USER_BIN_DIR="$HOME/.local/bin" ./install-user.sh
```

路径必须为绝对路径且不含换行。目录支持空格、中文及 shell 特殊字符。
如果 `.local/bin` 尚不在 PATH，可通过完整路径启动。安装器不会修改 shell 配置。

## 界面语言

界面有简体中文和英文两种。默认跟随系统：系统语言是中文（按 `LC_ALL`、`LC_MESSAGES`、
`LANG` 和 `LANGUAGE` 的第一项判断）显示中文，其他语言显示英文。打开设置（窗口右上角的
齿轮，或 Ctrl+,），在 **语言** 里选择跟随系统、中文或 English，改完立即生效。
切换之前已经保存的卡顿记录和执行记录，保持保存时的文字。

## 一次卸载所有已识别的用户版本，保留全部数据

先在托盘菜单退出 DevStats。普通麒麟用户
直接运行，不需要 `sudo`。卸载器只读检查全部候选安装和入口后才执行删除；
任一候选仍在运行、文件被修改、混入未知文件、标记异常、所有权异常或存在不安全
符号链接时，整次操作中止，任何版本都不先删除。不会结束进程。

```sh
# 查看全部待卸载版本、入口及保留的数据路径；不改动文件
~/.local/bin/devstats-uninstall --dry-run
# 一次卸载本次发现的所有用户安装版本，始终保留数据
~/.local/bin/devstats-uninstall
```

也可使用安装目录内的 `uninstall-user.sh`、解压包里的同名脚本，或单独复制出来的
脚本；独立脚本旁边不需要 `VERSION` 文件。命令目录自定义时使用该目录里的
`devstats-uninstall`。默认会发现：

- `~/.local/opt` 的直接子目录中，具有 DevStats 用户安装标记的所有版本。
- 本脚本所在的已安装目录、`DEVSTATS_USER_INSTALL_DIR` 或 `--install-dir`
  指定的安装，以及这些安装所在目录的直接同级版本。
- `~/.local/bin`、`DEVSTATS_USER_BIN_DIR` 以及已发现安装记录的命令目录中，
  `devstats`、`devstats-uninstall` 以及旧版 `devmon` 链接指向的安装及其同级版本。

每个已发现安装的路径记录还用于清理其自定义命令、XDG 菜单、图标和配置目录里的
登录启动项。按这些入口继续发现其他安装时只检查直接同级目录，并记录已访问路径，
不会递归搜索整个家目录、磁盘或普通子目录。没有安装标记的解压包、无关文件、
系统 `.deb` 安装和源码构建都保留。只有标记、包清单和文件校验全部通过的当前用户
安装目录才可删除。菜单和登录启动项必须与已识别版本生成的内容完全匹配；
未知入口明确提示保留；仍指向待卸载版本、内容被修改过的带安装标记菜单会使整次操作中止。

不再有入口指向的自定义旧安装可能无法自动发现。显式补充它的父目录：

```sh
./uninstall-user.sh --search-dir "$HOME/应用/DevStats 历史版本" --dry-run
./uninstall-user.sh --search-dir "$HOME/应用/DevStats 历史版本"
```

`--search-dir` 只检查所给目录的直接子目录；也可用
`--install-dir "$HOME/应用/devstats-0.7.2"` 提供一个已知安装作为发现入口。
这两个参数均可重复，含义是补充发现范围；卸载仍处理全部已识别用户版本。
旧的 0.7.1 / 0.7.2 安装没有路径记录时，自定义入口须沿用原来的
`DEVSTATS_USER_BIN_DIR`、`XDG_DATA_HOME`、`XDG_CONFIG_HOME`，以便清理它们。
未被识别的目录和入口不会推测删除。请先用 `--dry-run` 检查实际范围。

全部历史、设置、任务和缓存无条件保留，没有数据清除选项：

- 每个已知 `${XDG_CONFIG_HOME:-$HOME/.config}/devstats/settings.ini`。
- 每个已知 `${XDG_DATA_HOME:-$HOME/.local/share}/devstats/History/metrics.sqlite3`
  及其 `-wal`、`-shm`，同一数据目录中的 `Tasks/`、`Launchers/` 和其他文件。
- `DEVSTATS_DATA_DIR` 指定的自定义数据，以及所有未知或已知缓存目录。

同一数据目录可供多个版本共用，卸载不会修改它；不同 XDG 目录中的数据也都保留。
若安装目录与要保留的数据目录重叠，卸载会中止。没有安装可删除时为无操作，
保留解压包或独立脚本后可以重复检查。安装目录里的卸载脚本会随安装一同删除。

## 私有运行库与系统要求

包内 Qt 5.12.8、Qt SQLite 驱动、SQLite、C++ 运行库及直接动态依赖来自 Ubuntu
20.04 ARM64 软件包。可在 `share/runtime-manifest.json` 查阅逐文件哈希、原始
软件包版本、软件包哈希、源代码入口与外部库清单；版权在 `share/licenses`，
完整通用许可证在 `share/common-licenses`。DevStats 使用 MIT 许可证。
Qt 等库保持动态链接，允许用户以符合其许可证和相同 ABI 的库替换包内副本。
源代码入口指向每个原始版本，可从 Ubuntu / Launchpad 获取对应源码与发行补丁。

包内含 X11/xcb、Wayland、offscreen 平台插件、Wayland shell 插件、输入法插件、
图像插件与 SQLite 插件。`libexec/qt.conf` 将插件定位到本包；私有 Qt 不会误用
目标系统较新版本的 Qt 插件。启动器局部设置库搜索路径，应用初始化后还原原始
环境，因此从应用启动的终端和外部程序保持调用者的
LD_LIBRARY_PATH / Qt 环境（包括原本未设置或空值的区别）。

系统必须提供 AArch64 glibc 动态加载器 `/lib/ld-linux-aarch64.so.1`、glibc 2.31+
及 Linux `/proc`、`/sys`。图形界面需要可用的 X11 或 Wayland 会话及其托盘支持，
中文字体使用目标系统字体。GPU 的 DRI / EGL 厂商模块、内核驱动、字体文件、
桌面托盘、D-Bus 服务属于目标系统；私有运行库没有附带显卡驱动或字体包。
托盘和硬件电池显示须在实际麒麟电脑验证；QEMU/offscreen 验证不能替代此检查。

## 构建此包（维护者）

在 Linux 构建机先构建 glibc 2.31 基线的 ARM64 ELF，再运行：

```sh
scripts/package-linux-user.sh --binary build/linux-arm64/devstats \
  --sysroot /path/to/arm64-sysroot --overlay /path/to/runtime-overlay \
  --archives /path/to/original-arm64-debs --archives /path/to/overlay-debs \
  --output-dir ../../dist
```

构建需 Python 3.9+、readelf、dpkg-deb；安装和启动无需 Python。
构建器沿 ELF 的 DT_NEEDED 收集依赖，验证所有附带 ELF 为 AArch64、无构建 RPATH，
且要求的 GLIBC 符号不超过 2.31。原始 .deb 只读读取以生成可核查的来源信息。
仅排除 glibc 家族与加载器；平台插件需要的其余直接依赖均附带。
使用 Qt 官方建议的应用局部启动脚本和 qt.conf 部署方式：
https://doc.qt.io/qt-6/linux-deployment.html 、
https://doc.qt.io/qt-6/deployment-plugins.html 。

## 显式安装范围

需要把程序和入口都放入一个指定目录时，可执行 `./install-user.sh --prefix /绝对路径/DevStats`；版本目录为 `opt/devstats-1.0.0`，入口为 `bin/devstats`。卸载用 `./uninstall-user.sh --prefix /绝对路径/DevStats`，只检查这个前缀下的已验证安装，不扫描默认 HOME 目录；设置、历史、任务和缓存仍保留。前缀必须是无符号链接的规范绝对路径，不能为空或根目录。

## 卡顿诊断

打开 DevStats → 侧栏 **卡顿诊断**，或右键托盘 → **一键诊断**。

- **实时**：顶部横幅给出结论和依据，下方是处理器、内存、磁盘繁忙、响应延迟、温度、等待磁盘的进程 6 项指标，以及相关进程。其他用户的进程和内核线程也会列出。
- **一键诊断**：观察约 10 秒，区分持续和偶发的问题，再按平均 CPU、内存峰值或读写峰值排出相关进程。它不会关闭任何程序。
- **卡顿记录**：出现连续压力或短暂冻结时自动保存当时的证据，保留 31 天，可搜索，也可导出成 HTML 报告；页面右上角可关闭自动记录。
- **压力历史**：调度延迟、磁盘繁忙、I/O 等待、换出速率和 PSI 停顿的长期曲线。

“导出报告…”和“导出所选…”生成单个 HTML 文件，用浏览器直接打开或打印，不需要联网；原始数据附在页面末尾。报告包含应用名、PID 和资源指标，不含命令行和程序路径，文件仅自己可读写。

诊断只读取 `/proc`、`/sys`，不需要 root。内核没有 PSI 时改用调度等待、直接回收和磁盘延迟判断。进程排名表示当时相关，不等于确认是原因。

## 自动查杀

打开 DevStats → 侧栏 **自动查杀**，或右键托盘 → **自动查杀…**。默认关闭、没有预设规则。

1. 点 **添加规则…**，选一个正在运行的进程，或填写完整程序路径 / 精确程序名。在 **卡顿诊断** 或进程列表里右键进程也能直接加入。
2. 选择时机：**CPU 超过 X%，持续 Y 秒**（100% 等于占满 1 个核心，按单个进程判断），或只要运行就结束。
3. 选择方式：先发 SIGTERM，可选 N 秒后仍未退出则发 SIGKILL。SIGKILL 不给程序保存的机会，不需要可以关掉。
4. 打开顶部总开关并确认一次。之后规则自动执行，不再逐次询问。

每条规则实时显示状态（比如“已超限 18 / 30 秒 · 平均 92%”）。**执行记录**会确认程序是否真的退出了，保留最近 100 条（个人数据目录的 `process-rules-log.json`，权限 0600），升级和卸载都不会删除。关闭总开关立即生效。规则只在 DevStats 运行期间生效，需要开机生效时请在设置里打开登录后启动。

只能结束当前普通用户未提权的进程；桌面、会话、终端和 DevStats 自身受保护。不使用 root。每次发信号前都重新核对进程身份，并通过稳定进程句柄发送；内核不支持时会记录原因，不会退回按数字 PID 发送。用户内核 `5.4.96-24-kr9a0` 满足基础要求（Linux 5.1 起提供），实际权限以运行时检查为准。

从 0.7.x 升级后，旧规则保持原样：无 CPU 条件，不强制结束。

前缀只控制安装和卸载范围。运行时数据位置仍遵循 `XDG_CONFIG_HOME`、`XDG_DATA_HOME`、`XDG_CACHE_HOME` 与 `DEVSTATS_DATA_DIR`；需要完全隔离运行时应显式设置这些变量，无需改写 HOME。
