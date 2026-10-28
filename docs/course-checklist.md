# 课程交付检查单（Backup Studio 1.12.0）

| 项目 | 内容 |
| :--- | :--- |
| 检查单版本 | 1.9 |
| 检查日期 | 2026-10-28 |
| 对应代码基线 | GitHub `v1.11.0` 及当前工作区 `1.12.0` 源码 |
| 对应软件版本 | 1.12.0 |
| 说明 | 当前 Linux 验证见 `docs/test-report.md`；历史归档证据与当前结果按日期区分。 |

## 1 交付物清单

| # | 交付物 | 位置 | 状态 |
| ---: | :--- | :--- | :--- |
| 1 | 需求分析说明书 | `docs/requirements.md` | 已完成（同步至 1.12.0，含 FR-01..20） |
| 2 | 系统设计文档 | `docs/design.md` | 已完成（同步至 1.12.0，含自动备份、增量上传与 OpenSSL TLS） |
| 3 | 软件测试报告 | `docs/test-report.md` | 已完成（覆盖 6 大类 40+ 条用例，CTest 3/3、WSL 脚本 9/9 通过，`DEF-01` 已修复闭环） |
| 4 | UML 模型（StarUML） | `DataBackup.mdj`（6 张图，937 元素，已入库） | 已完成（同步至 1.12.0 基线，含自动备份、条目选择与 TLS/增量接口） |
| 5 | 测试证据 | `docs/evidence/`（按版本归档的构建日志、CTest、性能、截图） | 已归档材料见测试报告附录 D；Windows/WSLg 实机交互与 systemd 主机验收待补 |
| 6 | 图生成与校验工具 | `docs/tools/make-diagrams.py`、`docs/tools/validate-mdj.py` | 已入库 |
| 7 | 文档内嵌图源 | `docs/mermaid/*.mmd`（5 个） | 已完成 |
| 8 | 项目看板（Teambition） | — | **待补** |
| 9 | 成员贡献表 | 本文件第 4 节（真实 Git 统计） | 已完成 |
| 10 | 演示录像 / 证书 | `证书.png`（阿里云×电子科技大学云工开物训练营结课证书） | **证书已齐备**；2 分钟演示录像待补 |
| 11 | 发布安装包（客户端/服务端/一体包） | `Utils/package-wsl.sh`、`packaging/install-notes.md` | 构建入口与依赖声明（含 `adduser, openssl`）已验证；Windows 实机安装待补 |
| 12 | Windows（WSL2）安装器 | `packaging/windows/install-wsl.ps1`、`Install-BackupStudio.cmd` | 实现已入库；Windows 实机安装验收待补 |
| 13 | 容器交付 | `Dockerfile`、`docker-compose.yml` | 已完成：WSL2 Docker 29.1.3 实机构建与 compose 往返验收（`docs/evidence/docker-verify-190.log`） |

## 2 文档规范性检查

| 检查项 | 结果 | 说明 |
| :--- | :--- | :--- |
| 三份文档结构完整 | 通过 | 均含封面信息表、章节编号、术语、追溯、变更记录 |
| 版本一致 | 通过 | 三份文档与 `CMakeLists.txt` 均为 1.12.0；历史版本记录保留原日期 |
| 需求编号可追溯 | 通过 | FR-01..20 / NFR-01..10 ↔ 设计 4.x ↔ 用例 TC-* |
| 术语与依赖说明一致 | 通过 | 明确区分自研模块（BKP2 容器、RLE/Huffman/LZ77 压缩、ChaCha20/AES-256-CTR/SHA-256 加密）与第三方依赖（OpenSSL TLS、Qt 6） |
| 命令与选项与代码一致 | 通过 | 取自 `cli/main.cpp` 与 `cli/arguments.cpp` 的实际白名单（含 `watch`、`--include`、`--incremental`、`--tls`、`--tls-ca`） |
| 数据与格式与代码一致 | 通过 | 头部 112 字节、标志位、条目类型取自 `core/internal.hpp`；NBKP 消息 1–41（含增量消息 15–18）取自 `network/internal.hpp` |
| 用例编号与用例名一致 | 通过 | 测试报告 6 大类超过 25 条正式用例，与需求附录 A 追溯矩阵一致 |
| 作者与日期真实 | 通过 | 作者取自 Git 提交历史；日期为实际执行日期 |
| 结果可核对 | 通过 | 明确区分已完成验证项与受环境限制未完成项（Windows/WSLg 实机交互、真实 systemd 启停、需 root 权限设备节点还原） |

## 3 UML 建模（StarUML）

| 图 | 名称 | 元素 | 用途 |
| :--- | :--- | :--- | :--- |
| 类图 | Backup and network interfaces | `BackupEngine`、`BackupClient`、`BackupServer`、选项/结果类型（含 `automatic`、`includes`、`scanSource`、`bindAddress`、`tlsCertificate`、`login`、`deleteAccount`） | 设计文档第 3 节、公共接口说明 |
| 用例图 | 用例图 - Backup Studio | 2 参与者、12 用例、12 关联、8 «include» | 需求文档第 3 节 |
| 时序图 | 时序图 - 本地备份 | 4 生命线、9 消息（含 `includes` 过滤与 LZ77） | 设计 4.1、测试 TC-ALG |
| 时序图 | 时序图 - 本地还原 | 4 生命线、10 消息 | 设计 4.5、测试 TC-SEC |
| 时序图 | 时序图 - 远程上传 | 4 生命线、13 消息（含增量块复用与可选 TLS） | 设计 4.6/4.7/4.14、测试 TC-NET/INC/TLS |
| 部署图 | 部署图 - 客户端与备份服务器 | 2 节点、4 组件、1 «TCP» 通信路径 | 需求 2.3、设计 3.1 |

校验方式：`python3 docs/tools/validate-mdj.py DataBackup.mdj`，当前 937 个模型/
视图元素、2 393 条引用全部可解析，无重复 `_id`、无悬挂引用、无父子关系不一致。

## 4 成员贡献（截至 `v1.11.0` 及 `1.12.0` 工作区的真实统计）

| 成员 | Git 账号标识 | 已提交数 | 主要范围（按提交信息与当前工作区归类） |
| :--- | :--- | ---: | :--- |
| 黄博洋（组长） | `Byer`（19）、`ByerScipt`（5） | 24 | 核心归档与还原、自研压缩/加密算法、网络与账户存储、1.8.1 后端加固、1.9.1 条目选择修复、1.12.0 自动备份/增量上传/TLS 与安全边界修复、文档与构建 |
| 隋思奥（组员） | `E-Dawnize`（13）、`半梦`（3） | 16 | Qt 6 桌面界面与工作流（1.7.x）、1.8.0 LZ77 与安装包集成、1.9.0–1.10.0 条目选择与多维筛选、1.11.0 登录门控与预设、GCC 13 构建适配 |
| 甘智昊（组员） | `GGBang621`（1） | 1 | 1.6.2 取消传播、FIFO 竞态、账户库与关闭失败等缺陷修复与测试配合 |
| 合计 | — | 41（+1.12.0 工作区） | 2026-07-15（`v0.1`）— 2026-10-28（`v1.11.0` `d9c2466` + `1.12.0` 工作区） |

代码规模：仓库与工作区共 127 个文件，其中 `DataBackup/`、`GUI/`、`libs/`、`test/`
下的 54 个 C++ 源文件与头文件合计 15 786 行。

身份说明：`Byer` 与 `ByerScipt` 均为黄博洋；`E-Dawnize` 与 `半梦` 均为隋思奥；`GGBang621` 为甘智昊。

## 5 验证证据汇总

| 证据项 | 命令 / 位置 | 结果 |
| :--- | :--- | :--- |
| 格式检查 | `clang-format-18 --dry-run --Werror` | 通过 |
| CTest 全套（1.12.0） | `ctest --test-dir build --output-on-failure` | **3/3 全部通过**（`backup-tests`、`backup-cli-tests`、`backup-gui-tests`，含 `DEF-01` 修复） |
| WSL 启动与打包门禁 | `python3 test/test_build_wsl.py` | **9/9 全部通过** |
| 160 MiB 流式性能（1.12.0） | `Utils/run_performance_test.sh`（160 MiB / 128 MiB 限制，`index`/`none`/`none`） | **PASS**（备份约 1.81 s / 6 816 KiB，还原约 3.66 s / 6 652 KiB，校验一致；仅代表无压缩无加密条件） |
| LZ77 压缩对比与位翻转扫描 | `docs/evidence/lz77-comparison-180.log`、`lz77-fuzz-180.log` | 压缩率显著优于 RLE/Huffman；408 个位翻转变体全部拒绝 |
| 容器构建与远端往返 | `docs/evidence/docker-verify-190.log` | Docker 29.1.3 镜像构建与 compose 远端往返通过 |
| GUI 界面截图 | `docs/evidence/*.png`、`实验报告/figures/gui_*.png` | 已归档 |

## 6 课程点名工具的证据矩阵

| 工具 | 课程要求 | 当前证据 | 状态 |
| :--- | :--- | :--- | :--- |
| Teambition | 项目管理 | 仓库内无记录 | **缺**：需归档看板截图与任务/分工记录 |
| StarUML | UML 建模 | `DataBackup.mdj`（6 张图，937 元素，已入库） | **已有** |
| VSCode | IDE | `.vscode/settings.json`（compile_commands、C++17） | **部分**：缺 `launch.json` 与 `tasks.json` |
| make | 构建 | `Makefile`（configure/build/test/format-check/gui） | **已有** |
| gdb | 调试 | 无使用记录 | **缺**：需补一次真实调试会话记录 |
| git | 版本控制 | 真实提交、作者与分支见 Git 历史（41 次提交 + 1.12.0 工作区） | **已有** |
| cpplint | 代码检测 | 使用 clang-format 18 + clang-tidy | **缺 cpplint**：已在报告中说明等价工具，可按需补跑 |
| gtest | 单元测试 | 自研 `check()` 断言 + CTest + Python + Qt Test（6 类 46 条正式用例族） | **说明等价实现** |
| valgrind | 内存检测 | 无 | **缺**：可补 `valgrind --leak-check=full ./bin/backup-tests` |
| gprof / perf | 性能分析 | 现用 `/usr/bin/time -v` + `ulimit -v` 测时间与峰值 RSS | **部分** |

## 7 提交前门禁状态

| 门禁 | 状态 |
| :--- | :--- |
| 配置、格式检查、并行构建 | 1.12.0 全部通过，包含 OpenSSL TLS 与 Qt GUI 目标 |
| CTest | 1.12.0 为 3/3 全部通过（`DEF-01` 已修复闭环）；WSL 启动器与打包门禁 9/9 通过 |
| GUI 与安装包依赖 | 离屏回归与截图检查通过；服务端 `adduser, openssl` 与纯 CLI 客户端无 Qt 依赖检查通过 |
| 未完成实机验证项声明 | 已在文档中如实说明：Windows/WSLg 实机交互、真实系统服务 `systemd` 启动、受权限限制的设备节点还原未视为已完成验证 |

## 8 检查单变更记录

| 版本 | 日期 | 变更 | 依据 |
| :--- | :--- | :--- | :--- |
| 1.9 | 2026-10-28 | 同步 1.12.0：更新自动备份、增量上传、OpenSSL TLS、UID 筛选及各项修复；CTest 3/3 与 WSL 脚本 9/9 全通过；`DEF-01` 闭环；更新代码规模（54 个 C++ 文件 / 15,786 行）与 AI 训练营证书状态 | `CHANGELOG.md` 1.12.0、`docs/test-report.md`、`DataBackup.mdj` |
| 1.8 | 2026-10-28 | 同步 1.11.0 登录分区与预设、Docker 实机验收证据 | `v1.11.0`、`docs/evidence/docker-verify-190.log` |
| 1.7 | 2026-10-28 | 同步 1.10.0 条目选择类别与筛选扩展 | `v1.10.0` |
| 1.6 | 2026-10-28 | 同步 1.9.1 条目选择修复回归与 UML 模型 | `v1.9.1` |
| 1.0–1.5 | 2026-10-14..27 | 初始编写并同步 1.8.0–1.9.0 基线 | `CHANGELOG.md` |
