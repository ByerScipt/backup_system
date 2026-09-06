# Backup Studio 数据备份与还原系统

当前交付版本：`v1.4.0`。

面向 Linux/POSIX 的 C++17 课程项目。系统采用流式流水线，包含两种自研打包算法、
RLE/Huffman 压缩、XOR/Vigenère 教学加密、双 SHA-256 完整性校验、CLI、Qt6 GUI，
以及账号隔离的基础网络备份服务。

> XOR 与 Vigenère 仅用于算法教学，不适合保护生产环境中的敏感数据。网络版定位为
> 可信局域网演示，不提供 TLS。

## 快速构建

```bash
./scripts/build.sh
# 或者手工执行：
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

GUI 需要 Qt 6 Widgets（Debian/Ubuntu 软件包 `qt6-base-dev`）。建议同时安装
`fonts-noto-cjk`，界面会优先使用 Noto Sans CJK SC，并在其他平台依次选择思源黑体、
微软雅黑或苹方。未安装 Qt6 时 CMake 会给出警告，但仍构建 `backup-cli`、
`backup-server` 和测试。

```bash
build/backup-cli --version
build/backup-server --version
build/backup-gui
```

Qt6 GUI 采用侧边导航与卡片式任务界面，提供本地备份、本地还原、远程备份、远程
还原、备份历史和账号管理六个工作区。日志可复制，备份 ID 可双击复制，算法选项会
根据加密状态联动校验；端口使用清晰的“−/+”控件。字号以设备无关的 point 表示，
并随窗口可用空间在 1.0–1.5 倍之间响应式调整；低分辨率页面自动提供纵向滚动。
所有耗时操作仍调用公共核心并在后台线程执行。

## 工程结构

公开接口只有 `include/backup/core.hpp`（本地流水线）与 `network.hpp`（远程服务）；
实现按功能分文件，各文件均在 150 行左右，最大不超过 510 行：

- `src/core_*.cpp`（11 个）：`common`（错误/IO/临时文件）、`sha256_impl`、`entry`（扫描/元数据）、
  `pack`（顺序/索引打包）、`compress_rle`（含压缩调度）、`compress_huffman`、`crypto`、
  `archive`（包头/解码）、`restore`（fd 安全还原）、`engine`（BackupEngine 组装），共享声明在
  `src/core_internal.hpp`（`backup::detail`）。
- `src/net_*.cpp`（7 个）：`io`（Socket/帧协议）、`auth_store`（挑战登录/users.db）、
  `server`、`client_simple`（建连/注册/列表）、`client_upload`、`client_download`，
  共享声明在 `src/net_internal.hpp`（`backup::network::detail`）。
- `cli/`：`cli_args`（参数/密钥）、`commands`（本地/远程命令）、瘦 `main`（分发）。
- `gui/`：`theme`、`widgets`（卡片/任务）、`setup`（算法/服务器表单）、`dialogs`、
  `pages_local`、`pages_remote`、`pages_user`、`main_window`（组装），共享声明在
  `gui_common.hpp`，入口 `main` 只做 QApplication 装配。
- `tests/`：`helpers`（Fixture/树比较）、`test_combos`（18 种组合）、`test_security`
  （篡改/截断/竞态）、`test_network`，瘦 `test_main` 只做四段编排。
- `server/` 仍是单薄服务入口；`scripts` 放构建和性能复验，`deploy` 只放服务器配置示例。
课程报告、PDF/PPT、视频和证书始终位于同级 `../backup_system_delivery`。

## 本地命令

```bash
# 两种打包 × 三种压缩状态 × 三种加密状态均可组合
build/backup-cli backup ./data -o ./data.bak \
  --pack index --compress huffman --encrypt xor --key-file ./archive.key

build/backup-cli inspect ./data.bak
build/backup-cli restore ./data.bak -d ./restore --key-file ./archive.key
# 只有明确允许时才覆盖已有路径
build/backup-cli restore ./data.bak -d ./restore --key-file ./archive.key --overwrite
```

不提供 `--key-file` 时会使用终端无回显输入。归档默认保留源目录顶层名称，因此上例
会还原为 `./restore/data`。

## 网络演示

```bash
cp deploy/server.conf.example server.conf
build/backup-server --config server.conf

build/backup-cli user register --server 127.0.0.1:8848 --username alice
build/backup-cli remote-backup ./data --server 127.0.0.1:8848 \
  --username alice --name first --pack stream --compress rle --encrypt none
build/backup-cli remote-list --server 127.0.0.1:8848 --username alice
build/backup-cli remote-restore BACKUP_ID -d ./remote-restore \
  --server 127.0.0.1:8848 --username alice
```

服务器先写 `0600` 暂存文件，核对大小和 SHA-256 后原子提交；用户目录使用用户名
摘要隔离。账号密码不直接发送，登录采用随机挑战证明。

## 测试

```bash
ctest --test-dir build --output-on-failure
./scripts/run_performance_test.sh build/backup-cli
```

自动化测试在运行时创建真实符号链接、FIFO、Unix 套接字和空目录，覆盖 18 种算法
组合的完整树级比较，以及错误密码、篡改、截断、解压输出限界、路径竞态、路径穿越、
重复路径、边界长度、冲突覆盖与网络隔离。还原始终通过目录文件描述符和 `*at` 系列
接口逐级解析，默认提交不会覆盖竞态出现的目标。
性能证据写入同级的 `../backup_system_delivery/output/evidence/`。课程文档、PDF/PPT 与
提交打包工具全部放在独立的 `../backup_system_delivery/`，不混入本源码工程。

## 范围边界

本版本不实现 ACL、扩展属性、硬链接关系、增量备份、定时任务、实时监控或 TLS。
字符/块设备恢复需要足够权限；ctime 仅记录，POSIX 不支持任意恢复 ctime。
