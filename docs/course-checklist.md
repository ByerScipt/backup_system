# 课程交付检查单（Backup Studio 1.8.1）

| 项目 | 内容 |
| :--- | :--- |
| 检查单版本 | 1.4 |
| 检查日期 | 2026-10-23 |
| 对应代码基线 | `v1.8.1`（基于 `6e2529d` 的后端修复与性能优化） |
| 对应软件版本 | 1.8.1 |
| 说明 | 交付状态以仓库文件及已归档证据为依据；当前 CTest 结果见 `docs/evidence/ctest-detail-181.log`。 |

## 1 交付物清单

| # | 交付物 | 位置 | 状态 |
| ---: | :--- | :--- | :--- |
| 1 | 需求分析说明书 | `docs/requirements.md` | 已完成 |
| 2 | 系统设计文档 | `docs/design.md` | 已完成 |
| 3 | 软件测试报告 | `docs/test-report.md` | 已完成（保留历史缺陷 DEF-01；含 LZ77 与后端修复回归） |
| 4 | UML 模型（StarUML） | `DataBackup.mdj`（6 张图，已入库） | 已完成 |
| 5 | 测试证据 | `docs/evidence/`（按版本归档的构建日志、CTest、性能、截图） | 已归档材料见测试报告附录 D；其余待补 |
| 6 | 图生成与校验工具 | `docs/tools/make-diagrams.py`、`docs/tools/validate-mdj.py` | 已入库 |
| 7 | 文档内嵌图源 | `docs/mermaid/*.mmd`（5 个） | 已完成 |
| 8 | 项目看板（Teambition） | — | **待补** |
| 9 | 成员贡献表 | 本文件第 4 节（真实 Git 统计） | 已完成 |
| 10 | 演示录像 / 证书 | — | **待补** |
| 11 | 发布安装包（客户端/服务端/一体包） | `Utils/package-wsl.sh`、`packaging/install-notes.md` | 构建入口已入库；发布包与安装验收日志待补 |
| 12 | Windows（WSL2）安装器 | `packaging/windows/install-wsl.ps1`、`Install-BackupStudio.cmd` | 实现已入库；Windows 安装验收日志待补 |
| 13 | 容器交付 | `Dockerfile`、`docker-compose.yml` | 构建配置已入库；镜像及运行验收日志待补 |

## 2 文档规范性检查

| 检查项 | 结果 | 说明 |
| :--- | :--- | :--- |
| 三份文档结构完整 | 通过 | 均含封面信息表、章节编号、术语、追溯、变更记录 |
| 版本一致 | 通过 | 三份文档与 `CMakeLists.txt` 均为 1.8.1；本次基线为 `v1.8.1`，10-14 的 1.8.0 与 `3e928d3` 记录仅为历史证据 |
| 需求编号可追溯 | 通过 | FR-01..16 / NFR-01..10 ↔ 设计 4.x ↔ 用例 TC-* |
| 术语一致 | 通过 | 归档/packed/encoded/BKP2/NBKP 等术语在 requirements 1.3 定义，全文统一 |
| 命令与选项与代码一致 | 通过 | 取自 `cli/main.cpp` 与 `cli/arguments.cpp` 的实际白名单 |
| 数据与格式与代码一致 | 通过 | 头部 112 字节、标志位、条目类型取自 `core/internal.hpp`、`core/archive.cpp`；压缩 id 0–3、版本 1/2/3 与 LZ77 流格式见 `docs/design.md` 4.2/4.3.1 |
| 用例编号与用例名一致 | 通过 | 测试报告 TC-* 编号与需求附录 A 追溯矩阵一致 |
| 作者与日期真实 | 通过 | 作者取自 Git 提交历史；日期为实际执行日期 |
| 结果可核对 | 通过 | 结果引用 `docs/evidence/` 中的已归档材料；当前结果与历史缺陷按版本区分 |

## 3 UML 建模（StarUML）

| 图 | 名称 | 元素 | 用途 |
| :--- | :--- | :--- | :--- |
| 类图 | Backup and network interfaces | `BackupEngine`、`BackupClient`、`BackupServer`、选项/结果类型 | 设计文档第 3 节、公共接口说明 |
| 用例图 | 用例图 - Backup Studio | 2 参与者、10 用例、10 关联、6 «include» | 需求文档第 3 节 |
| 时序图 | 时序图 - 本地备份 | 4 生命线、9 消息 | 设计 4.1、测试 TC-ALG |
| 时序图 | 时序图 - 本地还原 | 4 生命线、10 消息 | 设计 4.5、测试 TC-SEC |
| 时序图 | 时序图 - 远程上传 | 4 生命线、13 消息 | 设计 4.6/4.7、测试 TC-NET |
| 部署图 | 部署图 - 客户端与备份服务器 | 2 节点、4 组件、1 «TCP» 通信路径 | 需求 2.3、设计 3.1 |

校验方式：`python3 docs/tools/validate-mdj.py DataBackup.mdj`，当前 860 个模型/
视图元素、2 218 条引用全部可解析，无重复 `_id`、无悬挂引用、无父子关系不一致。

模型维护方式：`docs/tools/make-diagrams.py` 可按同一份定义重建用例图、时序图与
部署图，并同步输出 `docs/mermaid/*.mmd`，保证 Markdown 内嵌图与 `.mdj` 一致。

## 4 成员贡献（截至 `3e928d3` 的历史 Git 统计）

| 成员 | 提交数 | 主要范围（按提交信息归类） |
| :--- | ---: | :--- |
| Byer | 19 | 核心归档与还原、压缩/加密算法、网络与账户、文档与构建 |
| E-Dawnize | 3 | UI 精简与工作流（1.7.0/1.7.1）、GCC 13 构建修复 |
| GGBang621 | 1 | 1.6.2 缺陷修复 |
| ByerScipt | 1 | 1.6.1 WSL 构建与启动命令 |
| 合计 | 24 | 2026-07-15（v0.1）— 2026-10-02（`3e928d3`） |

代码规模：纳入版本库 67 个文件，`DataBackup/`、`GUI/`、`libs/`、`test/`
的 C++ 源码合计约 11 058 行。

发现的小问题：`E-Dawnize` 在历史提交中使用过两个邮箱身份
（QQ 邮箱与 GitHub noreply），提交前建议统一 `user.email` 配置。

## 5 验证证据

1.8.1 的核心/网络、CLI、GUI 三项 CTest 均通过，逐项输出见
`docs/evidence/ctest-detail-181.log`。下表为 2026-10-14 的历史验收材料。

| 证据 | 命令 | 结果 |
| :--- | :--- | :--- |
| 格式检查 | `clang-format-18 --dry-run --Werror` | 通过 |
| 构建 | `./Utils/build-wsl.sh`（WSL2 Ubuntu 24.04） | 通过 |
| CTest | `ctest --test-dir build --output-on-failure` | 2/3 通过，`backup-gui-tests` 失败（DEF-01） |
| CLI 测试 | `python3 test/test_cli.py ...` | 通过（含 4 种压缩 × 3 种加密共 12 组往返） |
| WSL 启动器 | `python3 test/test_build_wsl.py` | 7/7 通过（1.166 s） |
| 性能与内存 | `Utils/run_performance_test.sh` | PASS（160 MiB / 128 MiB 上限，备份峰值 RSS 5 376 KiB、还原 5 440 KiB、往返 3 248 ms） |
| LZ77 压缩对比 | `docs/evidence/lz77-comparison-180.log` | 见 `docs/test-report.md` 4.3.7（两类样本均优于 RLE/Huffman） |
| LZ77 解码健壮性 | `docs/evidence/lz77-fuzz-180.log` | 两阶段各 204 个位翻转变体全部被拒，无崩溃/挂死/误接受（4.3.8） |
| GUI 离屏起停 | `BACKUP_GUI_CAPTURE=... backup-gui` | 退出码 0，截图 50 998 字节 |
| 日志与截图 | `docs/evidence/` | 已归档 |

未执行：`make test` 的完整串联（在 Windows Git Bash 中会调用 WSL 脚本），
以及 Windows/WSLg 交互式界面验收。

## 6 课程点名工具的证据矩阵

| 工具 | 课程要求 | 当前证据 | 状态 |
| :--- | :--- | :--- | :--- |
| Teambition | 项目管理 | 仓库内无记录 | **缺**：需归档看板截图与任务/分工记录 |
| StarUML | UML 建模 | `DataBackup.mdj`（6 张图，已入库） | **已有** |
| VSCode | IDE | `.vscode/settings.json`（compile_commands、C++17） | **部分**：缺 `launch.json`（gdb 调试配置）与 `tasks.json` |
| make | 构建 | `Makefile`（configure/build/test/format-check/gui） | **已有** |
| gdb | 调试 | 无使用记录 | **缺**：需补一次真实调试会话记录（命令与输出） |
| git | 版本控制 | 真实提交、作者与分支见 Git 历史；第 4 节保留早期统计 | **已有** |
| cpplint | 代码检测 | 使用 clang-format 18 + clang-tidy（`Checks: -*,clang-analyzer-*`） | **缺 cpplint**：需补一次 `cpplint` 运行报告，或在文档中说明等价工具与映射 |
| gtest | 单元测试 | 自研 `check()` 断言 + CTest + Python + Qt Test | **缺 gtest**：需补 gtest 目标（可用 FetchContent，与现有 CTest 并存） |
| valgrind | 内存检测 | 无 | **缺**：需补 `valgrind --leak-check=full ./bin/backup-tests` 报告 |
| gprof | 性能分析 | 无 | **缺**：需补一次编译期 `-pg` 分析 |
| perf | 性能分析 | 无（现用 `/usr/bin/time -v` 测时间与 RSS） | **缺**：需补 `perf record`/`perf report` 热点分析 |

### 6.1 建议补证顺序

1. **cpplint + valgrind**（成本低）：在 WSL 中安装并用现有测试二进制出具报告，
   结果并入 `docs/test-report.md` 第 4 节与本文档。
2. **perf / gprof**：对 `bin/backup-cli backup` 处理大文件的场景采样，
   输出热点函数表；可与 `Utils/run_performance_test.sh` 合并为一次执行。
3. **gtest**：为主要核心用例（算法组合、边界、路径安全）增加 gtest 目标，
   形成「gtest 单元层 + CTest 集成层」的双层结构。
4. **VSCode/gdb**：补 `.vscode/launch.json`（gdb 启动 `backup-cli`/`backup-tests`），
   并记录一次断点调试会话。
5. **Teambition**：导出看板截图，登记任务分解与成员分工；与第 4 节 Git 统计
   互相印证。
6. **DEF-01 修复**：见 `docs/test-report.md` 第 5 节；修复时补一条回归用例，
   并重新执行 GUI 测试。

## 7 提交前门禁状态

| 门禁 | 状态 |
| :--- | :--- |
| 配置、格式检查、并行构建 | 1.8.1 已通过，复现命令见测试报告附录 A |
| CTest | 1.8.1 为 3/3 通过（`docs/evidence/ctest-detail-181.log`）；1.8.0 的 Qt 6.4.2 历史缺陷 DEF-01 保留 |
| GUI | 1.8.1 离屏工作流测试通过；1.8.0 启动截图见 `docs/evidence/gui-smoke-180.png` |
| `.github/`、`.workflow` 不进入提交 | 通过（`.gitignore` 忽略） |
| `docs/` 随代码维护 | 已跟踪；本轮同步后端设计、需求与测试结果 |

## 8 待办与责任人

| # | 待办 | 建议责任人 | 期限 |
| ---: | :--- | :--- | :--- |
| 1 | 修复 DEF-01（起始页注册切换位移）并补回归用例 | E-Dawnize（1.7.1 界面提交者） | 下次提交 |
| 2 | 补 cpplint 与 valgrind 报告 | Byer | 下次提交 |
| 3 | 补 perf / gprof 热点分析 | Byer | 下次提交 |
| 4 | 补 gtest 单元测试目标 | Byer | 下次提交 |
| 5 | 补 `.vscode/launch.json` 与 gdb 调试记录 | ByerScipt | 下次提交 |
| 6 | 导出 Teambition 看板与分工记录 | GGBang621 | 演示前 |
| 7 | Windows/WSLg 交互式界面验收 | 全员 | 演示前 |
| 8 | 统一提交身份邮箱（E-Dawnize 两个身份） | E-Dawnize | 下次提交 |
| 9 | 在有 Docker 的机器上构建并运行服务端镜像，留存 `docker load`/远端往返证据 | 待定 | 演示前 |
| 10 | 在启用 systemd 的主机（或开启 systemd 的 WSL 发行版）验证服务开机自启与重启 | 待定 | 演示前 |

## 9 检查单变更记录

| 版本 | 日期 | 变更 | 依据 |
| :--- | :--- | :--- | :--- |
| 1.4 | 2026-10-23 | 清理仓库外产物引用；按已入库材料区分实现、交付物与验收证据；同步当前 CTest 门禁 | `docs/evidence/`、`Utils/package-wsl.sh`、`docs/test-report.md` |
| 1.3 | 2026-10-23 | 同步 1.8.1 代码与文档基线，记录后端复验 | `v1.8.1`、`docs/test-report.md`、`CHANGELOG.md` |
| 1.2 | 2026-10-14 | 交付物清单新增发布安装包、Windows WSL2 安装器与容器交付；待办新增容器与 systemd 验证项 | `Utils/package-wsl.sh`、`packaging/`、`Dockerfile` |
| 1.0 | 2026-10-14 | 首次编写；登记文档、UML、证据、工具矩阵与待办 | `docs/evidence/`、Git 历史、`AGENTS.md` |
