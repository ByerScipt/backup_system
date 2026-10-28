# Backup Studio 备份系统 软件测试报告

| 项目 | 内容 |
| :--- | :--- |
| 文档名称 | 软件测试报告 |
| 文档版本 | 1.6 |
| 被测版本 | 1.9.1（`CMakeLists.txt` `project(... VERSION 1.9.1)`） |
| 被测代码基线 | 与本文件所在 Git 提交的源码一致；历史测试按日期单独列出 |
| 测试日期 | 最新 2026-10-28；历史 2026-10-27、2026-10-23、2026-10-14 |
| 测试执行 | 项目组；复现命令见附录 A，已归档日志见附录 D |
| 结论 | Linux 完整回归结果见 2026-10-28 节；历史 Qt 6.4.2 布局缺陷 DEF-01 保留，Qt 6.2.4 通过不能证明其在其他 Qt 环境已修复 |

> 测试日志随文档保存在 `docs/evidence/`，附录 D 按版本列出。
> 日志中的目录占位符仅用于路径脱敏，测试输出、耗时和结果保持原样。

## 2026-10-28 条目选择修复与 Linux 复验

环境为 WSL2/Linux、普通用户 UID 1000、GCC 11.4、Qt 6.2.4，使用原版 POSIX
代码与 Release 构建，不采用历史 Cygwin 的兼容改动或跳过项。

| 用例 | 场景与测试入口 |
| :--- | :--- |
| TC-SEL-09 | 确定和取消结束对话框；取消保留原选择；`test/test_gui_jobs.cpp` 的 `driveEntrySelector`、`testEntrySelection` |
| TC-SEL-10 | 只选两个嵌套文件且顶层也有两项时仍保留部分选择；全选重开、清空后禁用确认、空目录可确认；`testEntrySelection` |
| TC-SEL-11 | 任务启动后修改源路径不影响当前归档；远程 GUI 选择后上传、下载还原且排除未选文件；`testEntrySelection`、`testUserWorkflow` |
| TC-SEL-12 | `src/` 与 `src` 归档和还原结果相同；`test/test_selection.cpp` 的 `testSelection` |

修复前，按钮测试报 `entry selector did not close`；尾斜杠用例报
`selected entry does not exist in the source: src/`。这两类复现现已纳入自动回归。
完整复验：clang-format 18.1.8、全量构建与离屏启动检查通过；CTest 核心/网络
7.16 秒、CLI 1.01 秒、GUI 7.70 秒，3/3 通过，无跳过项。WSL 启动器 7/7 通过。
另外补测选择性备份的全部 24 种打包/压缩/加密组合，中文及空格路径、还原内容和
未选条目排除均通过。160 MiB 文件在每进程 128 MiB 虚拟内存限制下往返成功，
SHA-256 一致；备份/还原峰值 RSS 均为 5088 KiB，耗时分别为 1.77/3.72 秒。
这些是单次验收值，不代表跨机器的性能保证。

复现入口为附录 A 的 CMake、format-check、CTest、WSL 启动器命令及
`Utils/run_performance_test.sh`。本次不新增日志或截图产物到仓库。

## 2026-10-23 后端复验

被测代码为 `v1.8.1`（`8fdcaa9`）。环境为普通用户 UID 1000、WSL2、GCC 11.4、
Qt 6.2.4，采用 Release 构建。

| 场景 | 结果与测试入口 |
| --- | --- |
| `--config` 位于参数前/后 | 两种顺序均监听显式指定的地址端口；`test/test_cli.py` |
| 配置路径是目录、重复选项、空存储参数 | 拒绝，不能静默按缺省配置启动；`test/test_network.cpp`、`test/test_cli.py` |
| 上传文件/目录、元数据文件/目录 fsync 故障 | 1–4 四个注入点全部清理未完成产物；`test/test_io.cpp` |
| 元数据尾随字节、非法摘要字符 | 拒绝且保留原文件；`test/test_network.cpp` |
| maxConnections=1 | 拒绝第二连接、回收首个连接、允许后续注册、带空闲客户端正常停机；`test/test_network_failures.cpp` |
| LZ77 输出设备写满 | 压缩和解压均报错；`test/test_io.cpp` |
| 既有完整回归 | 24 种算法组合、损坏/越界/权限、网络取消及 GUI 工作流均通过 |

发布门禁：Release 配置、clang-format 18 格式检查和全量构建通过。
CTest 核心/网络 6.97 秒、CLI 0.95 秒、GUI 7.62 秒，3/3 通过。
逐项输出与结果见 [1.8.1 CTest 日志](evidence/ctest-detail-181.log)。

性能相关实现见 `DataBackup/network/io.cpp` 的帧合并发送、
`DataBackup/network/client.cpp` 的块缓冲区复用，以及 `DataBackup/core/engine.cpp`
和 `DataBackup/core/archive.cpp` 的无变换阶段文件复用。内存与耗时验收可按附录 A
运行 `Utils/run_performance_test.sh`，历史测量见 4.3.5。

以下第 2、4–7 节保留 2026-10-14 的历史环境、结果及缺陷记录。

## 2026-10-27 条目选择复验（开发分支）

被测代码为 `dev/entry-selection`（已并入 `v1.8.1`）。本机没有 WSL 发行版、Docker
或 Linux 文件系统，本次复验在 MSYS2/Cygwin 后备环境完成：GCC 16.2、CMake 4.4.4、
Ninja，未安装 Qt 6，因此只覆盖核心与 CLI 两层。为让行为用例能在该环境运行，
在**仓库外的临时副本**中做了三处本地改动，均不进入仓库，也不改变 POSIX 平台行为：

| 本地改动 | 原因 |
| :--- | :--- |
| 只读句柄上的 `fsync()` 返回 `EACCES` 时按成功处理 | Cygwin 无法对只读句柄刷新缓冲；POSIX 平台可正常 `fsync` |
| 跳过 `testUnreadableSource`、`testMetadataFailure` | Cygwin 不按 POSIX 权限位拒绝访问，ACL 无法表达 Unix 权限 |
| 放宽 `scanSource` 的符号链接类型断言 | 无管理员权限时 Cygwin 创建的是文件副本，不是原生符号链接 |

| 检查项 | 结果与入口 |
| --- | --- |
| 全量编译（核心、网络、CLI、服务器、测试；`-DBACKUP_BUILD_GUI=OFF`） | 通过；GCC 16 在既有 LZ77 用例 `test_compress.cpp:283` 报 1 条数组越界告警（该代码本轮未改动，1.8.1 的 GCC 11 基线未触发） |
| 格式检查 `clang-format 18.1.8 --dry-run --Werror`（49 个源文件） | 通过 |
| `backup-tests --filesystem`（[1/4]，含条目选择用例） | 通过，跳过 3 项（见上表） |
| `test/test_cli.py`（含 `--include` 与白名单拒绝） | 通过 |
| `backup-tests` [2/4]–[4/4]（24 组合、失败模式、网络） | 未执行：Cygwin 无法把还原条目 `chown` 到其他属主，用例在元数据阶段中止 |
| GUI 构建、`backup-gui-tests` 与离屏启动截图 | 未执行：本机无 Qt 6 |

条目选择用例（`test/test_selection.cpp`、`test/test_cli.py`）：

| 用例 | 场景 | 结果 |
| :--- | :--- | :--- |
| TC-SEL-01 | 空选择等价于整目录；归档、预览与还原的条目数与元数据一致 | 通过 |
| TC-SEL-02 | 只选中嵌套文件，祖先目录 `docs`、`docs/manual` 一并归档 | 通过 |
| TC-SEL-03 | 选中目录保留整棵子树（`src`） | 通过 |
| TC-SEL-04 | 两条不相邻分支（`top.txt` 与 `src/nested/deep.bin`） | 通过 |
| TC-SEL-05 | 空目录与符号链接条目 | 通过（符号链接以文件副本建夹具，类型断言已放宽） |
| TC-SEL-06 | 非规整重复路径 `./docs//manual/../manual/guide.txt` 规整去重后只归档一次 | 通过 |
| TC-SEL-07 | 只选硬链接别名时退化为普通文件；成对选择时保持硬链接（`st_ino` 相同、`st_nlink` 为 2） | 通过 |
| TC-SEL-08 | 缺失条目、绝对路径、`..`、空串、`.` 被拒绝且不生成归档；`scanSource` 的路径顺序、类型、大小与 mtime 与目录树一致 | 通过 |
| TC-CLI-07 | `--include` 只归档选中项并保留结构；`inspect`/`restore`/`remote-restore` 拒绝该选项；缺值报错 | 通过 |

未覆盖：GUI 选择对话框的勾选、筛选、摘要与「源目录变化清空选择」由
`test/test_gui_jobs.cpp` 的 `testEntrySelection` 覆盖，本次未运行；WSL 全量 CTest、
离屏 GUI 截图与性能项仍待补（见 `docs/course-checklist.md` 第 8 节）。

## 1 概述

### 1.1 测试目标

验证 Backup Studio 1.9.1 是否满足 `docs/requirements.md` 的功能与非功能需求，
重点覆盖：归档管线与 24 种算法组合（含新增 LZ77）、归档健壮性与还原安全、
账户隔离与网络传输、CLI 参数校验、桌面端工作流，以及性能与内存约束。

### 1.2 测试范围

| 纳入范围 | 不纳入（本次环境受限） |
| :--- | :--- |
| 核心归档、还原、压缩、加密、摘要 | Windows/WSLg 交互式界面验收（需真实 Windows 桌面） |
| 文件系统元数据、硬链接、特殊文件 | MSYS2/Cygwin 平台（仅作为无 WSL 时的后备手段） |
| NBKP 协议、账户库、并发与超时 | 大规模压力/长时间稳定性测试（超出课程范围） |
| CLI 参数与提示、GUI 离屏工作流 | 第三方安全审计（自研算法仅做 KAT 与边界验证） |
| 性能与峰值内存（160 MiB 样本） | |

## 2 测试环境（2026-10-14 历史记录）

| 项 | 值 |
| :--- | :--- |
| 宿主系统 | Windows + WSL2 |
| 发行版 | Ubuntu 24.04（Linux 内核，ext4 文件系统） |
| 编译器 | GCC 13.3.0（`-Wall -Wextra -Wpedantic`） |
| 构建系统 | CMake 3.28.3，`CMAKE_BUILD_TYPE=Release`，并行 20 任务 |
| 桌面依赖 | Qt 6.4.2（Widgets/Test/Network）、Noto Sans CJK 字体 |
| 图形平台 | `QT_QPA_PLATFORM=offscreen`（离屏） |
| 构建账户 | `builder`（UID 1000，非 root，权限用例按普通用户执行） |
| 构建文件系统 | WSL Linux 的 ext4 文件系统；由 `Utils/build-wsl.sh` 分配独立构建副本 |

## 3 测试策略与用例组织

| 层次 | 载体 | 说明 |
| :--- | :--- | :--- |
| 单元/集成 | `bin/backup-tests`（CTest `backup-tests`） | 核心与网络，四个阶段顺序执行 |
| CLI 集成 | `test/test_cli.py`（CTest `backup-cli-tests`） | 参数校验、往返、服务器关闭 |
| GUI 集成 | `bin/backup-gui-tests`（CTest `backup-gui-tests`） | Qt Test 离屏运行 |
| 构建脚本 | `test/test_build_wsl.py` | WSL 启动器逻辑与失败保留 |
| 性能 | `Utils/run_performance_test.sh` | 时间与峰值内存，摘要比对 |
| 起停验收 | `Utils/build-wsl.sh` 末尾的离屏截图 | 进程可启动且画面非空 |

断言点静态统计（本次基线）：`check(...)` 239 处（核心/网络/CLI 辅助），
`require(...)` 72 处（GUI）。

## 4 测试执行与结果（2026-10-14 历史记录）

### 4.1 构建与格式检查

```
== format check ==      clang-format-18 --dry-run --Werror   通过（无输出）
== build ==             cmake --build build --parallel 20    通过（全部目标）
                        产物：bin/backup-cli、bin/backup-server、
                              bin/backup-gui、bin/backup-tests、bin/backup-gui-tests
```

### 4.2 CTest 汇总

| # | 测试 | 结果 | 耗时 |
| ---: | :--- | :--- | ---: |
| 1 | `backup-tests` | **通过** | 18.66 s |
| 2 | `backup-cli-tests` | **通过** | 0.40 s |
| 3 | `backup-gui-tests` | **失败** | 0.48 s |
| | 合计 | 2/3 通过（66.7%） | 19.54 s |

`backup-tests` 原始输出片段：

```
[1/4] SHA-256, cipher vectors, and LZ77 compression
[2/4] 24 complete-tree algorithm combinations
[3/4] corruption, bounds, conflicts, and path races
[4/4] account-isolated network roundtrip and session recycling
backup-server listening on 0.0.0.0:34813
backup-server listening on 0.0.0.0:40055
backup-server listening on 0.0.0.0:40055
All backup-system tests passed.
```

`backup-cli-tests` 原始输出：

```
CLI argument validation and restore-option roundtrips passed.
```

### 4.3 用例执行明细

#### 4.3.1 核心与网络（`backup-tests`，通过）

| 用例 ID | 用例 | 期望 | 结果 |
| :--- | :--- | :--- | :--- |
| TC-CRYPTO-01 | SHA-256、ChaCha20、AES-256-CTR 已知答案向量（含流偏移与续算） | 与标准向量一致 | 通过 |
| TC-ALG-01..24 | 2 种打包 × 4 种压缩 × 3 种加密的完整目录树往返 | 目录树比较一致 | 通过 |
| TC-LZ77-01 | LZ77 往返：空输入、1–2 字节、2 MiB+ 多块重复数据、1 MiB 高熵数据（存储块回退）、压缩/解压进度与取消 | 字节一致且不膨胀 | 通过 |
| TC-LZ77-02 | 手工构造 LZ77 流：字面量块、重叠匹配、长匹配扩展字节、存储块 | 解码结果正确 | 通过 |
| TC-LZ77-03 | 非法 LZ77 流（16 类）：头、声明大小、块标志、块大小、负载尺寸、截断、零/超界距离、越界匹配、块内及流尾部多余数据 | 全部拒绝且给出原因 | 通过 |
| TC-LZ77-04 | LZ77 版本门控：LZ77 归档写为版本 3，其余算法保持版本 2，伪造版本 2 + 压缩 id 3 的归档被拒 | 版本与拒绝理由正确 | 通过 |
| TC-SEC-01 | 错误口令 | 拒绝且不创建还原目录 | 通过 |
| TC-SEC-02 | 损坏归档（篡改负载/摘要） | `inspect`/`restore` 拒绝 | 通过 |
| TC-SEC-03 | 截断归档 | 拒绝 | 通过 |
| TC-SEC-04 | 非法算法标识与已退役加密 id | 拒绝并给出原因 | 通过 |
| TC-SEC-05 | 变换与索引边界（零/越界偏移、中央目录越界） | 拒绝，不越界读写 | 通过 |
| TC-SEC-06 | 路径攻击（绝对路径、`..`、重复路径、符号链接逃逸） | 拒绝，不写到目标目录之外 | 通过 |
| TC-SEC-07 | 还原冲突与 `--overwrite` | 默认拒绝；显式覆盖成功 | 通过 |
| TC-SEC-08 | 冲突预览 | 条目与冲突快照正确，不占用路径 | 通过 |
| TC-SEC-09 | 还原竞态（目标被替换为符号链接/文件） | 拒绝并保留原内容 | 通过 |
| TC-SEC-10 | 扫描期源文件被替换为 FIFO | 不阻塞、拒绝提交 | 通过 |
| TC-SEC-11 | 归档输出位于源目录内/已存在 | 拒绝 | 通过 |
| TC-SEC-12 | 输出父目录符号链接 | 拒绝且不写入 | 通过 |
| TC-FS-01 | 不可读源文件 | 上报失败 | 通过 |
| TC-FS-02 | 元数据还原失败（属主/权限/时间戳） | 上报失败，不静默忽略 | 通过 |
| TC-FS-03 | 普通文件硬链接 | 关系保留（BKP2 v2） | 通过 |
| TC-IO-01 | 归档 `fsync` 失败注入 | 报错、不发布、不泄漏描述符 | 通过 |
| TC-IO-02 | 服务器存储 `fsync` 失败注入 | 不提交、目录无残留 | 通过 |
| TC-IO-03 | 管线磁盘写满（缓冲关闭失败） | 上报失败 | 通过 |
| TC-IO-04 | 各阶段取消（含最终提交前、预览、等待网络 I/O） | 取消生效且不提交 | 通过 |
| TC-FS-04 | 源到 FIFO 竞态 | 见 TC-SEC-10 | 通过 |
| TC-NET-01 | 服务器配置加载与非法配置拒绝 | 拒绝非法项 | 通过 |
| TC-NET-02 | 账户注册/登录/隔离（Bob 看不到 Alice 的备份） | 隔离成立 | 通过 |
| TC-NET-03 | 登录证明与挑战请求 ID 匹配 | 不匹配即拒绝 | 通过 |
| TC-NET-04 | 登录期间账户被另一会话注销 | 待定挑战不能复活账户 | 通过 |
| TC-NET-05 | 有备份的账户注销 | 拒绝 | 通过 |
| TC-NET-06 | 上传往返（大小/摘要校验） | 摘要一致 | 通过 |
| TC-NET-07 | 上传中途取消 | 不提交、无残留 | 通过 |
| TC-NET-08 | 下载到既有符号链接 | 拒绝且不覆盖链接 | 通过 |
| TC-NET-09 | 列表在无权限账户目录下 | 报错而非「空列表」 | 通过 |
| TC-NET-10 | 服务器重启后持久化 | 列表仍可见 | 通过 |
| TC-NET-11 | 会话回收（断开后连接计数归零） | 注销可成功 | 通过 |
| TC-NET-12 | 无响应/畸形响应服务器（脚本化） | 超时或报错，不挂死 | 通过 |
| TC-NET-13 | 存储目录与文件权限（0700/0600） | 权限正确 | 通过 |
| TC-NET-14 | 账户库不可读/损坏 | 拒绝操作，不覆盖原库 | 通过 |
| TC-NET-15 | `bind_address`：合法字面量生效、非 IPv4（`localhost`、`999.1.1.1`、`::1`、空值、端口后缀）在配置与 `--bind` 两处均被拒绝，且服务器只监听指定地址 | 通过 |

#### 4.3.2 CLI 集成（`test_cli.py`，通过）

| 用例 ID | 用例 | 结果 |
| :--- | :--- | :--- |
| TC-CLI-01 | 端口/连接数/监听地址非法值（`0`、`65536`、`-1`、`+80`、`80junk`、`80.5`、` 80`、溢出；`localhost`、`999.1.1.1`、`::1`、`127.0.0.1:8848`、空值） | 通过 |
| TC-CLI-02 | 非本命令选项、重复选项、多余位置参数 | 通过 |
| TC-CLI-03 | 4 种压缩 × 3 种加密共 12 组备份-还原往返、`--overwrite`、非法压缩值与重复选项拒绝 | 通过 |
| TC-CLI-04 | 空闲连接与半发送连接下 `SIGTERM` 及时退出 | 通过 |
| TC-CLI-05 | `--help` 退出码 | 通过 |
| TC-CLI-06 | 服务器/客户端版本与用法输出可用 | 通过 |

#### 4.3.3 桌面端（`backup-gui-tests`，1 项失败）

| 用例 ID | 用例 | 结果 |
| :--- | :--- | :--- |
| TC-GUI-01 | 起始页登录/注册切换不移动标题、选择器与字段 | **失败（DEF-01）** |
| TC-GUI-02 | 对话框在主窗口内居中、随页面切换保持可见 | 通过 |
| TC-GUI-03 | 历史列表搜索/排序/选择/整 ID 复制 | 通过 |
| TC-GUI-04 | 与真实服务器注册/上传/列表/下载；加密本地往返 | 通过 |
| TC-GUI-05 | 多任务关闭与取消等待清理 | 通过 |

失败原始输出：

```
This plugin does not support propagateSizeHints()
GUI workflow: login/register switch moved the title, selector or fields
```

#### 4.3.4 WSL 启动器（`test_build_wsl.py`，通过）

```
Ran 7 tests in 1.166s
OK
```

覆盖：既有目录保留、自定义 GUEST_DIR 与 GUI 启动命令、缺省目录唯一、
源缺失不创建目标、非法目标拒绝、符号链接目标拒绝、构建失败保留副本。

#### 4.3.5 性能与内存（`Utils/run_performance_test.sh`，TC-PERF-01，通过）

被测样本：160 MiB 稀疏文件；每进程虚拟内存上限 128 MiB；打包 `index`、
不压缩、不加密。

| 指标 | 实测值 |
| :--- | :--- |
| 归档字节数 | 167 772 508 |
| 备份峰值 RSS | 5 572 KiB |
| 还原峰值 RSS | 5 296 KiB |
| 往返耗时 | 3 849 ms |
| 源/还原 SHA-256 | `61b5d2e2…42519d`（一致） |
| 结果 | PASS |

结论：在远小于输入体积的内存上限内完成，说明管线按缓冲流式处理而非整体载入。

#### 4.3.6 GUI 离屏起停与截图

```
mkdir -p output/evidence
QT_QPA_PLATFORM=offscreen BACKUP_GUI_CAPTURE=output/evidence/gui-smoke.png timeout 10s ./bin/backup-gui
退出码 0；截图 docs/evidence/gui-smoke-180.png 50 998 字节（非空）
```

#### 4.3.7 LZ77 压缩效果与开销（`docs/evidence/lz77-comparison-180.log`）

命令：`bin/backup-cli backup SRC -o X.bak --pack stream --compress ALG`，`ALG`
取 `none`/`rle`/`huffman`/`lz77`，其余参数相同；`/usr/bin/time -v` 记录墙钟
时间与峰值 RSS。两个样本均以 `diff -r` 验证 LZ77 还原结果与源目录一致。

样本 A：混合语料 25 575 042 字节（Qt6Core / libcrypto / libstdc++ 二进制 +
libstdc++ 头文件）：

| 算法 | 归档字节 | 相对 none | 耗时 |
| :--- | ---: | ---: | ---: |
| none | 25 673 681 | — | 0.246 s |
| rle | 24 189 926 | 94.2% | 0.346 s |
| huffman | 20 520 959 | 79.9% | 0.856 s |
| lz77 | 8 954 934 | 34.9% | 1.081 s |

样本 B：高度重复文本 108 468 000 字节：

| 算法 | 归档字节 | 相对 none | 耗时 |
| :--- | ---: | ---: | ---: |
| none | 108 468 307 | — | 0.935 s |
| rle | 109 315 694 | 100.8% | 1.026 s |
| huffman | 58 950 607 | 54.3% | 2.355 s |
| lz77 | 545 941 | 0.5% | 0.460 s |

峰值内存（LZ77，`/usr/bin/time -v`）：样本 A 11 824 KiB、样本 B 9 660 KiB。
分块编码使内存与归档总大小无关（1 MiB 块 + 哈希链）；随机/已压缩数据以存储块
原样保留，因此不会出现「压缩后更大」的情况。

结论：LZ77 在两个样本上都明显优于此前的 RLE/Huffman（样本 A 为 Huffman 的
44%，样本 B 为其 1%），代价是压缩时间略增。

#### 4.3.8 LZ77 解码健壮性（位翻转扫描，`docs/evidence/lz77-fuzz-180.log`）

补充 TC-LZ77-03 的位翻转扫描：样本为 3 574 字节 LZ77 归档（112 字节头 +
3 462 字节负载），对负载每 17 字节取 1 字节逐位取反，每个变体执行一次
`restore`：

| 阶段 | 变体 | 被接受 | 崩溃/超时 | 拒绝原因分布 |
| :--- | ---: | ---: | ---: | :--- |
| 保持 encoded 摘要不变 | 204 | 0 | 0 | payload 摘要不匹配 204 |
| 重算 encoded 摘要（解码器真正执行） | 204 | 0 | 0 | 截断字面量 89、解压后摘要不匹配 85、非法距离 28、非法头 1、输入流截断 1 |

结论：畸形压缩流不会造成崩溃、挂死或错误数据落地；要么被 LZ77 解码器的边界
校验拒绝，要么落到 packed 摘要校验上。

#### 4.3.9 发布安装包（TC-PKG-01..12，待补证）

发布实现已入库：`Utils/package-wsl.sh`、`CMakeLists.txt`、`packaging/`、
`Install-BackupStudio.cmd`、`Dockerfile` 与 `docker-compose.yml`。
构建入口及产物说明见 `README.md` 的 Packaging 节，安装方式见
`packaging/install-notes.md`。

仓库尚未归档发布包及其安装、卸载、容器运行、systemd 自启和 Windows 安装验收日志，
本报告将这些交付验收项标为待补证。

## 5 缺陷记录

### DEF-01 起始页注册模式导致标题与字段整体上移（打开）

| 项 | 内容 |
| :--- | :--- |
| 发现用例 | TC-GUI-01（`testAccountAlignment`） |
| 严重程度 | 一般（界面布局，不影响数据正确性） |
| 可复现性 | 100%（本次连续 3 次执行均失败） |
| 环境 | WSL2 Ubuntu 24.04 / Qt 6.4.2 / offscreen，窗口 1320×900 |

实际测量（登录 → 注册切换后）：

| 控件 | 切换前 (x,y,w,h) | 切换后 (x,y,w,h) | 变化 |
| :--- | :--- | :--- | :--- |
| 标题 `startTitle` | 321, 257, 938, 47 | 321, 222, 938, 47 | y −35 |
| 模式选择器 `accountMode` | 321, 340, 240, 48 | 321, 305, 240, 48 | y −35 |
| 密码字段 `serverPassword` | 918, 480, 341, 48 | 918, 445, 341, 48 | y −35 |

1.8.0 复核（2026-10-14）：在含 LZ77 改动的构建上复现，三项控件几何值与上表
逐项一致；改用 `git archive HEAD`（`3e928d3`）构建后测得相同数值，确认该缺陷
与 1.8.0 的压缩改动无关。

根因分析：

1. `GUI/widgets.cpp` 的 `makeCard()` 对卡片内容调用
   `setAlignment(Qt::AlignVCenter)`，内容块在卡片内**垂直居中**。
2. 注册模式显示「确认密码」行后，`QFormLayout` 的该行高度
   （48 px 行高 + 22 px 行距 = 70 px）从未显示变为显示，内容块变高 70 px。
3. 内容块居中，因此整体上移 70/2 = 35 px，与实测一致。
4. `pages_account.cpp` 虽然对确认字段与其标签设置了
   `QSizePolicy::setRetainSizeWhenHidden(true)`，但 **QFormLayout 不保留
   隐藏行的高度**，因此预期中的「占位」没有生效。

影响：注册/登录切换时页面内容跳动；功能可用，但违反 1.7.1 的界面稳定性
目标（`CHANGELOG.md` 1.7.1 明确要求「切换时不重排」）。

建议修复方向（任选其一，需补一条回归用例后实施）：

- 让确认密码行始终占位：用固定高度的容器承载该行，在登录模式放入等价
  高度的占位控件；
- 或将该卡片改为顶部对齐（`AlignTop`），并在其它卡片上保持一致的视觉；
- 或使用 `QFormLayout::setRowVisible` 之外的自定义行高固定策略。

### 历史缺陷（已修复，摘自 `CHANGELOG.md` 真实记录）

| 版本 | 缺陷 | 修复 |
| :--- | :--- | :--- |
| 1.6.2 | 空闲/半连接会话导致服务器停机挂起 | 传播取消并轮询 |
| 1.6.2 | 不可读账户库被误当作空库覆盖 | 拒绝并保留原文件 |
| 1.6.2 | 源文件被替换为 FIFO 时阻塞 | 无阻塞打开并拒绝 |
| 1.6.2 | 磁盘写满/缓冲关闭失败被忽略 | 全链路检查关闭失败 |
| 1.6.0 | 硬链接关系丢失 | BKP2 v2 保存硬链接 |
| 1.6.0 | 元数据还原失败被静默忽略 | 必须上报 |
| 1.5.0 | 旧 XOR/Vigenère 弱加密 | 替换为 ChaCha20 / AES-256-CTR，旧 id 拒绝 |
| 1.4.0–1.1 | 解压越界、还原路径逃逸、输出非原子 | 边界校验、FD 约束、原子提交 |

## 6 覆盖分析与追溯

| 需求 | 用例 | 结果 |
| :--- | :--- | :--- |
| FR-01/14/15 | TC-CRYPTO-01、TC-ALG-01..24、TC-LZ77-01..04、TC-FS-01..03 | 通过 |
| FR-02/03/05 | TC-SEC-01..09、TC-CLI-03 | 通过 |
| FR-04 | TC-SEC-02/03/04、TC-CLI-06 | 通过 |
| FR-06/07/11 | TC-NET-01..05、TC-NET-11、TC-GUI-04 | 通过 |
| FR-08/09/10 | TC-NET-06..10、TC-NET-12、TC-GUI-04 | 通过 |
| FR-12 | TC-NET-01、TC-NET-13、TC-CLI-04 | 通过 |
| FR-13 | TC-IO-04、TC-GUI-05 | 通过 |
| FR-16 | TC-NET-02、TC-NET-13、TC-NET-14 | 通过 |
| FR-17 | TC-SEL-01..12、TC-CLI-07 | 当前 Linux 复验见 2026-10-28 节；10-27 为历史部分验证 |
| NFR-01 | TC-PERF-01（4.3.5） | 通过 |
| NFR-02/04 | TC-SEC-01..05、TC-IO-01..03、TC-LZ77-01/03 | 通过 |
| NFR-03 | TC-SEC-06..12、TC-NET-03 | 通过 |
| NFR-05 | WSL2 构建与全量 CTest | 通过 |
| NFR-06 | `format-check` | 通过 |
| NFR-07/09 | TC-GUI-01（**DEF-01**）、TC-GUI-02/03/05 | 1 项失败 |
| NFR-08 | 全部 CTest 离屏运行、TC-WSL-01..07 | 通过 |
| NFR-10 | TC-IO-01/02、TC-NET-07 | 通过 |

覆盖不足与说明：

- 属主/权限还原用例在非 root 账户下对部分场景自动跳过（本次 `builder`
  为普通用户，跳过情况未单独记录为失败）。
- Windows/WSLg 交互式界面验收尚未在本机执行，需在真实 Windows 桌面完成。
- MSYS2/Cygwin 后备环境未参与 10-14 的验证；2026-10-27 的条目选择复验在该环境完成，
  本地改动与跳过项已在该节列出。

## 7 结论与遗留问题（2026-10-14 历史记录）

1. 构建、格式检查、核心、网络、CLI、WSL 启动器与性能项全部通过；
   归档格式、协议与安全约束未发现缺陷。
2. 唯一失败项 DEF-01 属界面布局回归，已定位到 `makeCard` 垂直居中与
   `QFormLayout` 隐藏行不占位的组合原因，修复方向明确，待下一迭代实施。
3. 遗留：发布包及安装验收、WSLg 交互验收、Teambition 看板与成员分工记录、cpplint /
   gtest / valgrind / gprof / perf 等课程点名工具的证据仍需补齐，
   详见 `docs/course-checklist.md`。

## 附录 A 复现命令

```bash
# 一键（Git Bash → WSL2）：格式检查 + 并行构建 + CTest + 离屏 GUI 截图
./Utils/build-wsl.sh

# 单独执行
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBACKUP_BUILD_GUI=ON
cmake --build build --target format-check
cmake --build build --parallel
ctest --test-dir build --output-on-failure
python3 test/test_cli.py bin/backup-cli bin/backup-server
python3 test/test_build_wsl.py
QT_QPA_PLATFORM=offscreen ./bin/backup-gui-tests
mkdir -p output/evidence
BACKUP_GUI_EVIDENCE_DIR="$(pwd)/output/evidence" QT_QPA_PLATFORM=offscreen ./bin/backup-gui-tests
DELIVERY_ROOT="$(pwd)/output/benchmark" MEMORY_KIB=131072 SIZE_MIB=160 \
  ./Utils/run_performance_test.sh bin/backup-cli

# 发布安装包（Git Bash → WSL2）：从已发布标签构建，输出由命令生成
git switch --detach v1.8.1
DELIVERY_ROOT="$(pwd)/output/delivery" ./Utils/package-wsl.sh
```

## 附录 B 环境变量与证据目录

| 环境变量 | 用途 |
| :--- | :--- |
| `BACKUP_GUI_CAPTURE` | 离屏启动后保存主窗口截图 |
| `BACKUP_GUI_EVIDENCE_DIR` | GUI 测试保存登录/注册对齐截图（DEF-01 现场） |
| `DELIVERY_ROOT` | 性能脚本将证据写入所选目录下的 `output/evidence/`；这些输出由执行命令生成 |

## 附录 C 文档变更记录

| 版本 | 日期 | 变更 | 依据 |
| :--- | :--- | :--- | :--- |
| 1.6 | 2026-10-28 | 同步 1.9.1，增加按钮、部分选择、空选、快照与尾斜杠回归，按当前 Linux 环境复验 | `test/test_gui_jobs.cpp`、`test/test_selection.cpp`、`CHANGELOG.md` 1.9.1 |
| 1.5 | 2026-10-27 | 新增 2026-10-27 条目选择复验：后备环境、3 处本地改动、TC-SEL-01..08 与 TC-CLI-07 结果；明确 GUI 与 [2/4]–[4/4] 未执行 | `dev/entry-selection`、`docs/evidence/entry-selection-msys-190.log` |
| 1.4 | 2026-10-23 | 文档引用统一为仓库文件、发布标签和可复现命令；归档 1.8.1 CTest 日志并脱敏历史日志路径；安装包验收按入库证据重标 | `v1.8.1`、`docs/evidence/`、`Utils/package-wsl.sh` |
| 1.3 | 2026-10-23 | 同步 1.8.1，记录配置、同步故障、元数据、容量回收与 LZ77 写入失败回归；区分当前复验与历史环境缺陷 | `v1.8.1`、`docs/evidence/ctest-detail-181.log` |
| 1.2 | 2026-10-14 | 新增发布安装包检查项（TC-PKG-01..12）、TC-NET-15 与 TC-CLI-01 的 `--bind` 用例 | `Utils/package-wsl.sh`、`libs/network.hpp` |
| 1.1 | 2026-10-14 | 同步 1.8.0：CTest 与性能数据更新为本次实机结果；新增 TC-LZ77-01..04、4.3.7 压缩效果对比与 4.3.8 解码健壮性扫描；DEF-01 补充 1.8.0 复核 | WSL2 实机日志（附录 D）、`CMakeLists.txt` 1.8.0、`CHANGELOG.md` |
| 1.0 | 2026-10-14 | 首次编写；填写本机真实执行结果与 DEF-01 | WSL2 实机日志（附录 D）、`CHANGELOG.md` |

## 附录 D 已归档日志与截图

文本日志只替换了具体机器的路径：`$SOURCE_DIR` 表示仓库根目录，`$BUILD_ROOT`
表示独立构建副本，`$BUILD_DIR` 表示构建目录，`$BIN_DIR` 表示可执行文件目录，
`$FIXTURE_DIR` 表示测试数据目录，`$RUNTIME_DIR` 表示图形运行时目录。
这些是日志阅读占位符，不是运行命令的前置配置。

| 文件 | 内容 |
| :--- | :--- |
| `docs/evidence/entry-selection-msys-190.log` | 2026-10-27 条目选择复验的原始输出（环境、本地改动、编译、[1/4]、CLI 与格式检查） |
| `docs/evidence/ctest-detail-181.log` | 1.8.1 三项 CTest 的逐项输出、耗时与通过结果 |
| `docs/evidence/build-wsl-180.log` | 1.8.0 一键脚本日志（format-check、构建、CTest） |
| `docs/evidence/ctest-detail-180.log` | 1.8.0 三项 CTest 详细输出（含 DEF-01） |
| `docs/evidence/lz77-comparison-180.log` | 4.3.7 压缩对比与 LZ77 峰值内存原始输出 |
| `docs/evidence/lz77-fuzz-180.log` | 4.3.8 位翻转扫描（两阶段 408 个变体） |
| `docs/evidence/performance-summary-180.txt` | 1.8.0 性能与内存汇总 |
| `docs/evidence/wsl-launcher-180.log` | 1.8.0 WSL 启动器测试输出 |
| `docs/evidence/gui-smoke-180.png` | 1.8.0 离屏启动截图 |
| `docs/evidence/build-wsl-20261014.log` | 一键脚本完整日志（format-check、构建、CTest） |
| `docs/evidence/ctest-detail.log` | `backup-tests` 与 CLI 测试输出 |
| `docs/evidence/performance-summary.txt` | 性能与内存汇总 |
| `docs/evidence/gui-smoke.png` | 离屏启动截图 |
| `docs/evidence/account-login-1320.png` | DEF-01 现场：登录模式截图 |
