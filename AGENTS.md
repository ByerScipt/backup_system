# Backup Studio 工作约定

## 实现

- 公共接口放在 `include/backup/`，实现细节留在 `src/core/`、`src/network/`。
  CLI 和 Qt GUI 通过公共接口复用业务逻辑。
- 文件名用 `snake_case`，类型用 `PascalCase`，函数与字段用 `camelCase`，
  私有成员加尾随下划线，常量用 `kPascalCase`。
- C++ 格式以 `.clang-format` 为准：4 空格、80 列、独行大括号、控制流加花括号。
  使用 clang-format 18；YAML 保持 2 空格。
- 公共接口说明参数约束、返回/异常、线程及资源生命周期。复杂算法解释不变量与边界，
  错误处理说明原因；避免逐句翻译代码。作者、日期和修改记录采用真实 Git 历史。
- 文件句柄和临时文件采用 RAII；文件系统错误必须与“不存在”“空集合”区分。
- 归档格式变更需版本化，保留旧格式读取测试；还原路径始终受目标目录 FD 约束。

## 验证和交付

- 行为修改在公共接口补复现用例，再修复并运行受影响测试；文件系统测试使用临时目录，
  网络测试使用本机临时端口。权限用例以普通用户运行，跳过情况必须报告。
- 修改归档、网络或公共接口后运行 CTest；界面修改另做 Qt 启动与交互验收。
- 需求和验收范围见 `docs/requirements.md`；格式、模块和协议变更同步
  `docs/design.md`；用例与真实结果同步 `docs/test-report.md`。
- 课程交付检查见 `docs/course-checklist.md`。项目看板、成员贡献、证书、演示录像
  使用真实记录；历史产物不作为当前版验证证据。

## 代码结构速览

- 分层：`include/backup/core.hpp`、`include/backup/network.hpp` 是唯一公共接口；
  `src/core/`（归档、压缩、加密、文件系统）、`src/network/`（协议、账户、存储、
  客户端）为实现；`cli/`、`gui/`、`server/` 只做编排；`tests/` 覆盖三层。
- 归档管线：扫描目录树 → 打包（`stream` 每条目带元数据；`index` 尾部偏移表）→
  压缩（none/RLE/Huffman）→ 加密（none/ChaCha20/AES-256-CTR）→ 提交单个最终文件。
  格式为自有 `BKP2`（头 112 字节），版本 1 为旧布局、版本 2 增加普通文件硬链接；
  packed 数据与 encoded 负载各有 SHA-256 摘要，读入后先校验再落盘。
- 还原安全：目标目录 FD 约束全部路径解析；`O_NOFOLLOW`、`AT_SYMLINK_NOFOLLOW`
  逐层打开；拒绝路径逃逸与竞态替换；默认不覆盖既有路径，`--overwrite` 显式开启；
  每条目遍历提交，不做整目录回滚；元数据（属主/权限/时间戳）失败必须上报。
- 网络：分帧 TCP + 挑战-响应登录；每账户独立存储目录；上传按声明大小与
  SHA-256 校验后才提交；每连接一个工作线程，受连接上限与超时约束；协议无 TLS，
  仅用于可信网络，机密性由归档加密单独保证。
- 依赖边界：核心不引入外部压缩/加密库；只用 C++17、`std::filesystem` 与 POSIX。
  Qt 6 Widgets 仅 GUI 需要，缺失时 CMake 自动跳过 GUI 并给出提示。
- 可移植性：Linux 为主要目标；平台差异用 `#if defined(__linux__)`、
  `#if defined(__CYGWIN__)`、`#ifdef __APPLE__` 局部守卫并写明原因，
  不得让平台分支改变 POSIX 平台的行为。

## 项目管理规则

- 版本与变更：版本号在 `CMakeLists.txt` 的 `project(... VERSION)`；行为与格式变更
  记入 `CHANGELOG.md`；归档格式变更必须版本化并保留旧版本读取测试。
- 文档四件套：`docs/requirements.md`（需求与验收范围）、`docs/design.md`
  （格式、模块、协议）、`docs/test-report.md`（用例与真实结果）、
  `docs/course-checklist.md`（交付检查）。`docs/` 已在 `.gitignore` 中忽略。
- CI 门禁（`.github/workflows/`，ubuntu-24.04）：配置 → `format-check` → 并行构建 →
  CTest → 离屏启动 GUI 并校验截图非空。提交前本地跑通同样步骤。
- 测试约定：行为修改先补复现用例；文件系统用例用临时目录，网络用例用本机临时端口；
  权限用例以普通用户运行，跳过必须打印原因；禁止用历史产物充当当前验证证据。
- 协作：作者、日期与修改记录取自真实 Git 历史，不伪造贡献记录；看板、证书与
  演示录像同步真实进展。

## 本机构建环境

- 首选 WSL2（Ubuntu 24.04，由官方 cloud-image rootfs 导入）作为 Linux 构建与运行
  环境，一键脚本：`./scripts/build-wsl.sh`（同步到 ext4 → `format-check` → 并行构建 →
  CTest → 离屏 GUI 截图校验）。等价手工命令：
  `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` → `cmake --build build --parallel` →
  `ctest --test-dir build --output-on-failure`；GUI 用
  `QT_QPA_PLATFORM=offscreen BACKUP_GUI_CAPTURE=/tmp/backup-studio.png ./build/backup-gui`
  验证启动。
- 构建必须落在 ext4 文件系统（`~/backup_system`），不要用 `/mnt/*`：DrvFs 无法
  表达 Unix 权限与属主，硬链接/ inode 语义也与 CI 不一致，会让权限类用例失真。
- 依赖：`build-essential cmake ninja-build pkg-config qt6-base-dev qt6-base-dev-tools
  qt6-wayland libgl1-mesa-dev clang-format-18 fonts-noto-cjk`。构建账户必须是非 root
  （权限用例以普通用户运行），且 UID 宜为 1000 以匹配 WSLg 运行时目录属主。
- GUI 可直接用 WSLg 显示：`wsl -d Ubuntu-24.04 -- ~/backup_system/build/backup-gui`；
  xcb、wayland、offscreen 三种平台插件均已验证可渲染并截图。
- 上游 1.6.0 的 `cli/arguments.hpp` 使用了 `uint16_t` 但未包含 `<cstdint>`，
  在 GCC 13 上 CLI 无法编译；已补该头文件，构建与 CI 均需要它。
- MSYS2 的 msys（Cygwin）环境只是无 WSL 时的后备手段：需要
  `-DCMAKE_CXX_FLAGS=-D_GNU_SOURCE`，且平台差异（atime 不可复现、原生符号链接
  透明跟随、ACL 无法表达 Unix 权限、FIFO 不能硬链接）会导致部分用例跳过。
  为此写过的 Cygwin 兼容改动不进入仓库。
