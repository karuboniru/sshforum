# systemd 安装、隔离与 RPM 打包

目标平台为使用 systemd ≥ 252 的 Fedora Linux。程序以 MIT 许可发布。默认端口为 TCP 2222，保留匿名 SSH 访问。构建发生在 toolbox 中；最终服务在目标宿主机上由 systemd 直接运行，不通过 toolbox 启动。

## 1. 构建 SRPM 与 RPM

在项目根目录运行：

```bash
# 仅首次需要安装构建工具，全部装在 toolbox 中
bash scripts/build-toolbox.sh --install
bash scripts/build-srpm.sh --install

# 后续只生成源码包
bash scripts/build-srpm.sh
```

生成文件：

```text
dist/rpmbuild/SOURCES/sshforum-0.1.0.tar.gz
dist/rpmbuild/SPECS/sshforum.spec
dist/rpmbuild/SRPMS/sshforum-0.1.0-6.*.src.rpm
```

源码归档使用显式文件清单，包含代码、测试、安装文档、unit、spec 和构建脚本；不包含 `data/`、数据库、主机密钥、构建目录或 Git 元数据。脚本检查 CMake/spec 版本一致，归档文件时间由 `SOURCE_DATE_EPOCH` 控制，默认归零；这不承诺整个 SRPM/RPM 二进制完全可复现。

从生成的 SRPM 构建二进制包：

```bash
toolbox run -c fedora-toolbox-45 rpmbuild --rebuild \
  --define "_topdir $PWD/dist/rpmbuild" \
  dist/rpmbuild/SRPMS/sshforum-0.1.0-6.*.src.rpm
```

`%check` 会运行 SQLite、TUI 和真实 SSH 测试；测试使用临时目录和随机高端口。输出在 `dist/rpmbuild/RPMS/`。目标系统版本不同，应使用匹配目标 Fedora 版本的 toolbox 和 `TOOLBOX_CONTAINER=容器名` 构建，不应直接安装为更新 glibc/libssh 构建的包。

## 2. 安装与首次启动

普通 Fedora 宿主机上安装主包（不必安装 debuginfo/debugsource）：

```bash
sudo dnf install ./dist/rpmbuild/RPMS/$(uname -m)/sshforum-0.1.0-6.*.$(uname -m).rpm
sudo systemctl enable --now sshforum.service
systemctl status sshforum.service
sudo journalctl -u sshforum.service -f
```

Fedora Atomic/rpm-ostree 宿主机使用 `sudo rpm-ostree install 包路径`，重启进入新 deployment 后再执行 `systemctl enable --now`。包的首次安装通过标准 systemd preset 宏遵循宿主策略；包本身不主动启动服务。升级用标准 systemd scriptlet 重启正在运行的实例，已有 SSH 连接会断开；卸载会停止服务，保留论坛数据。

| 路径 | 内容与权限 |
| --- | --- |
| `/usr/bin/sshforum` | RPM 管理的可执行文件 |
| `/usr/lib/systemd/system/sshforum.service` | RPM 管理的 unit |
| `/usr/share/doc/sshforum/` | README、安装说明、临时测试脚本 |
| `/usr/share/licenses/sshforum/LICENSE` | MIT 许可 |
| `/var/lib/sshforum/` | 服务内部看到的持久化目录，0700 |
| `/var/lib/private/sshforum/` | DynamicUser 在宿主机上管理的实际私有数据目录 |

无需 `useradd`、固定 UID 或手动 `chown` 给某个动态 UID。首次启动由 systemd 建立 state 目录，程序生成数据库和 Ed25519 私钥；`UMask=0077` 限制新文件访问。SQLite 的 `-wal`、`-shm` 文件也位于同一目录。

如需从源码安装而非 RPM，请使用 `/usr` 前缀，并启用 unit 安装：

```bash
cmake -S . -B build-system -DCMAKE_INSTALL_PREFIX=/usr \
  -DSSHFORUM_INSTALL_SYSTEMD=ON \
  -DSSHFORUM_SYSTEMD_UNIT_DIR=/usr/lib/systemd/system
cmake --build build-system
sudo cmake --install build-system
sudo systemctl daemon-reload
sudo systemctl enable --now sshforum.service
```

此处编译仍应在匹配目标系统的 toolbox 中进行；`sudo cmake --install` 是宿主安装动作。建议优先使用 RPM，让包管理器跟踪文件与依赖。

## 3. 目录和进程限制

`DynamicUser=yes` 配合 `StateDirectory=sshforum` 保留数据，同时避免将持久文件暴露给之后重用该 UID 的进程。`ProtectSystem=strict` 将宿主文件系统只读化，持久写入仅允许论坛 state 目录；`PrivateTmp` 提供隔离的临时文件空间。

服务不再用 tmpfs 覆盖 `/etc`、`/var`、`/run`，也不再逐个 bind 系统配置文件。DynamicUser 仍受普通 Unix 权限和 SELinux 限制；普通用户本就可读的系统配置与公共文件现在也对服务可读。家目录以及 `/boot`、`/efi`、`/opt`、`/srv`、`/mnt`、`/media` 仍被隐藏或禁止访问。`ProtectSystem=strict` 保护写入，并不宣称隐藏所有可读文件。

`0.1.0-2` 移除了这层配置文件白名单，以避免 Fedora SELinux 拒绝 systemd 的 `init_t` 域在临时 `/etc` 中创建 `ld_so_cache_t` 挂载目标，进而触发 `226/NAMESPACE`。见 [systemd 上游同类问题](https://github.com/systemd/systemd/issues/36224)。无需关闭 SELinux、改策略或安装额外目录骨架。

已安装旧版时，可先用 drop-in 修复，不必重建程序：

```bash
sudo systemctl edit sshforum.service
```

写入以下内容以清空旧 unit 的列表设置：

```ini
[Service]
TemporaryFileSystem=
BindReadOnlyPaths=
```

然后执行：

```bash
sudo systemctl daemon-reload
sudo systemctl restart sshforum.service
sudo journalctl -u sshforum.service -n 30 --no-pager
```

这不会改变论坛数据或主机密钥。新版 RPM 已直接移除这些设置；升级后可以仅删除上述临时 drop-in 中的两行，保留其他本地设置。在 Fedora 44 上重建 SRPM 会得到 `.fc44` 包，不要安装为 Fedora 45 运行库构建的 `.fc45` 二进制包。

其余约束包括清空 capabilities、禁止提权、私有设备、禁用可写可执行内存、限制命名空间和系统调用、保护内核与 cgroup，并隐藏其他用户的进程。服务无需系统 shell、设备访问、系统 SSH 配置或用户凭据。

网络保留客户端入站连接；不使用 `PrivateNetwork=yes`。绑定规则只放行 TCP 2222，禁止主动 `connect()`。监听地址应是数值 IP，不依赖 DNS 或 NSS socket。IPv4/IPv6 的实际可用性取决于监听地址；默认 `0.0.0.0` 监听 IPv4。

systemd、内核的 seccomp/namespace/cgroup-BPF 支持以及 SELinux 策略决定限制是否完整生效。安装后应结合日志和 `systemd-analyze security` 验证；静态评分不能证明服务在所有限制下已成功启动。不要以关闭 SELinux 作为排错步骤。

## 4. 使用 systemd-run 测试相同约束

先在宿主机安装 RPM。以下命令不会修改正式 unit，测试使用 `sshforum-test.service`、`127.0.0.1:22222` 和独立的 `sshforum-test` state 目录：

```bash
# 输出一条可直接复制执行的 sudo systemd-run 命令；不会启动服务
bash scripts/test-systemd.sh --print

# 执行同一条命令（会通过 sudo 请求宿主权限）
bash scripts/test-systemd.sh --run

# RPM 安装后，也可直接使用包内脚本
bash /usr/share/doc/sshforum/test-systemd.sh --run

ssh -tt -p 22222 anonymous@127.0.0.1
sudo journalctl -u sshforum-test.service -f
sudo systemd-analyze security sshforum-test.service
```

脚本从源码 unit（在源码树中执行时）或已安装 unit 读取 `[Service]` 的限制，传给 `systemd-run --property=…`，避免维护另一套较弱的手写参数。它仅替换实例名、端口、数据目录、启动参数，禁用自动重启，并设置 10 分钟自动退出。`--collect` 在停止后回收临时 unit；state 数据和日志仍保留，便于复查。

检查挂载命名空间和进程约束：

```bash
pid=$(systemctl show sshforum-test.service --property=MainPID --value)
test "$pid" -gt 0
sudo ls -la "/proc/$pid/root/etc" "/proc/$pid/root/var" "/proc/$pid/root/run"
sudo cat "/proc/$pid/status"
sudo systemctl show sshforum-test.service \
  -p DynamicUser -p StateDirectory -p ProtectSystem -p NoNewPrivileges \
  -p CapabilityBoundingSet -p SystemCallFilter -p SocketBindAllow -p SocketBindDeny
```

以 root 查看 `/proc/PID/root` 可检查服务看到的目录，但不等同于以动态 UID 验证每一个文件访问权限。实际功能测试应包括：连接、发帖、回帖、刷新、退出，再重新启动测试实例确认数据和主机密钥保留。

测试结束：

```bash
sudo systemctl stop sshforum-test.service
# 确认已停止后，如不再需要测试帖子和测试密钥，手动删除这两个测试路径
sudo rm -rf -- /var/lib/private/sshforum-test /var/lib/sshforum-test
```

## 5. 配置、备份和升级

`0.1.0-3` 首次启动会自动将原始数据库迁移到 schema v1，增加匿名作者字段和身份 HMAC 密钥。旧帖子保留原内容、时间和 ID，作者显示 `Anonymous`。升级前按下述方式备份；密钥随数据库备份，无需额外 writable path 或 unit 配置。恢复同一数据库可以保持相同连接输入的匿名标识。


用 `sudo systemctl edit sshforum.service` 创建 drop-in。例如只监听回环地址并改为 2223：

```ini
[Service]
Environment=SSHFORUM_BIND=127.0.0.1 SSHFORUM_PORT=2223
SocketBindAllow=
SocketBindAllow=ipv4:tcp:2223
SocketBindAllow=ipv6:tcp:2223
```

然后运行 `sudo systemctl daemon-reload` 和 `sudo systemctl restart sshforum.service`。不能只改端口环境变量而保留旧的 `SocketBindAllow`。无需编辑 RPM 提供的 unit；将来的包升级不会覆盖 `/etc/systemd/system/sshforum.service.d/` 下的本地配置。

不建议变更数据目录：state 目录、工作目录、显式路径和访问规则必须一致。端口小于 1024 可能需要额外 capability，与当前清空 capabilities 的设计不符。远程访问还需按管理员选定的接口和区域开放宿主防火墙端口；包不自动修改防火墙。

备份时先停止服务，再以 root 保存整个 `/var/lib/private/sshforum/`；保留主机密钥以避免 SSH 指纹改变。卸载不删除此目录。恢复时保持目录 0700、私钥 0600，不将动态 UID 固化为备份恢复规则；若从其他机器复制，可在服务停止时将整个 state 目录及内容设为 root 所有，下一次启动由 systemd 重新分配所有权。

参考：[systemd.exec 官方说明](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html)、[systemd-run](https://www.freedesktop.org/software/systemd/man/latest/systemd-run.html)、[Fedora systemd scriptlet 规范](https://docs.fedoraproject.org/en-US/packaging-guidelines/Scriptlets/#_systemd)。
