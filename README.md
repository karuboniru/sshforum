# sshforum

基于 **C++20、libssh 和 SQLite** 的 SSH 匿名论坛服务器。连接后直接进入全屏 TUI，帖子采用「主帖 + 平铺回复」结构，所有人显示为 Anonymous，没有注册、账户或权限分级。

## 构建与启动

Linux 环境，需要 C++20 编译器、CMake ≥ 3.20、libssh ≥ 0.10、SQLite3。项目提供 toolbox 构建脚本；Python/Paramiko 只用于 SSH 集成测试。

```bash
# 如果还没有 toolbox 容器，先创建一个
toolbox create -c fedora-toolbox-45

# 安装依赖、构建并运行测试（安装操作发生在 toolbox 内）
bash scripts/build-toolbox.sh --install

# 已安装依赖时
bash scripts/build-toolbox.sh

# 在项目目录启动服务，默认监听 0.0.0.0:2222
toolbox run -c fedora-toolbox-45 ./build/sshforum
```

使用其他现有容器时可设置 `TOOLBOX_CONTAINER=容器名`。toolbox 和宿主机共享项目目录；无需在宿主机安装开发库。

另开终端连接：

```bash
ssh -tt -p 2222 anonymous@localhost
```

用户名任意，标准 SSH 客户端的 `none` 认证直接成功，不需要输入密码。主动使用 password、publickey 或 keyboard-interactive 的客户端也会被接受，不查询系统用户、密码或 authorized_keys。SSH 协议本身的加密、签名和握手由 libssh 处理。

第一次连接时仍有标准 SSH 主机指纹确认，这是客户端验证服务器身份。服务器首次启动自动生成 Ed25519 主机密钥，后续复用同一文件。

```bash
./build/sshforum --bind 127.0.0.1 --port 2222 \
  --db data/forum.db --host-key data/ssh_host_ed25519_key --max-sessions 128
./build/sshforum --help
```

如果宿主机没有兼容的运行库，以上命令也应通过 `toolbox run -c fedora-toolbox-45` 执行。远程连接使用服务器地址，监听端口需能从客户端访问。

## 操作

| 页面 | 按键 | 操作 |
| --- | --- | --- |
| 帖子列表 | ↑ / ↓ 或 k / j | 选择帖子 |
| 帖子列表 | PgUp / PgDn | 翻页选择 |
| 帖子列表 | Enter | 进入选中帖子 |
| 帖子列表 | n | 新建帖子 |
| 列表 / 帖子 | r | 从数据库刷新 |
| 帖子详情 | ↑ / ↓ 或 k / j | 上下滚动 |
| 帖子详情 | PgUp / PgDn | 翻页 |
| 帖子详情 | a | 匿名回帖 |
| 帖子详情 | b | 返回列表 |
| 标题编辑 | Enter | 进入正文编辑 |
| 正文编辑 | Enter | 换行 |
| 正文编辑 | Ctrl+D | 提交 |
| 编辑 | ← / → | 前后移动光标 |
| 正文编辑 | ↑ / ↓ | 按显示行移动光标 |
| 编辑 | Home / End | 移到当前显示行的行首 / 行尾 |
| 编辑 | Ctrl+Home / Ctrl+End | 移到当前输入内容开头 / 结尾 |
| 编辑 | Backspace / Delete | 删除光标前 / 后的字符 |
| 编辑 | Esc | 取消 |
| 帖子列表 | q | 退出 |
| 任意页面 | Ctrl+C | 退出 |

支持中文 UTF-8 输入、光标位置插入与删除、窗口大小调整；编辑时显示真实终端光标，正文视口随光标滚动。新回复会将帖子顶到列表前方。另一位用户发帖或回帖后，按 `r` 查看。标题最多 120 字节，正文和每条回复最多 16 KiB，不接受空白内容。列表展示最近活跃的 200 个帖子。

## 结构与协程

主题详情显示主帖发布时间（`Posted`）和最后回复时间（`Last reply`），每条回复也显示自己的发布时间；尚无回复时显示 `No replies yet`。时间采用 UTC（末尾 `Z`），按 `r` 刷新最后回复时间。

```text
src/main.cpp          命令行选项与启动
src/server.cpp        libssh 回调、非阻塞连接、poll 调度器
include/sshforum/task.hpp  C++20 coroutine Task / RAII
src/tui.cpp           TUI 状态机、增量键盘解析、终端渲染
src/store.cpp         SQLite 持久化、事务、参数绑定
tests/                存储、TUI、真实 SSH 端到端测试
```

每个 SSH 连接由一个 C++20 协程负责，从密钥交换、认证、等待 shell 请求，到读取输入、发送输出及关闭连接。等待网络进展时通过 `co_await` 让出执行权，由单线程 `poll` 循环继续调度；没有每连接线程，也没有启动系统 shell。SSH 的 PTY 请求只记录终端尺寸，显示由 ANSI/VT100 转义序列完成。

SQLite 使用 WAL、外键、prepared statements 和事务。数据库访问保持同步，适合小型服务；写事务和较大的帖子渲染可能短暂阻塞事件循环。认证/进入界面超时为 30 秒，空闲连接超时 30 分钟，慢客户端输出有缓冲上限；这些是资源边界，不是账户权限。

服务仅实现论坛终端，不提供 exec 命令执行、SFTP、端口转发。帖子内容输出前过滤控制字符，防止终端转义注入。数据仅保存帖子和回复，不保存登录名或密码。当前版本没有附件、搜索、删除、管理面板或自动推送；正文很长时需滚动阅读。

## 数据与测试

生产安装、DynamicUser 隔离服务、`systemd-run` 临时测试和 SRPM/RPM 构建见 [安装与打包说明](docs/packaging.md)。项目采用 [MIT 许可](LICENSE)。

默认数据在 `data/forum.db`，主机密钥在 `data/ssh_host_ed25519_key`。两者应持久保存。停止服务后备份整个 `data/` 目录即可；运行中备份 SQLite 应使用 SQLite 的备份工具，不只复制主数据库文件。`Ctrl+C` / `SIGTERM` 停止服务并释放连接。

```bash
toolbox run -c fedora-toolbox-45 ctest --test-dir build --output-on-failure
```

`store` 验证持久化、排序、输入边界、并发和事务；`tui` 验证导航、编辑、UTF-8 与控制字符处理；`ssh_smoke` 启动临时服务器，通过 Paramiko 验证开放认证、双客户端操作、窗口调整、终端恢复和重启持久化。测试使用独立临时数据，不污染实际论坛。

libssh API 参考：[官方服务端文档](https://api.libssh.org/stable/group__libssh__server.html)。
