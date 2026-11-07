# Cova — 产品需求文档（PRD）

**版本：** 1.0  
**状态：** Draft / 架构设计阶段  
**产品名称：** Cova  
**产品类型：** C/C++ 构建系统、包管理器及跨平台工具链管理器  
**核心实现语言：** C++26  
**产品定位：** Cross-Compilation-First, Cargo-Inspired C/C++ Development Toolchain

---

# 1. 产品概述

## 1.1 产品简介

Cova 是一个面向现代 C/C++ 开发的开源项目管理、构建、依赖管理与工具链管理系统。

Cova 旨在解决 C/C++ 生态长期存在的构建配置复杂、依赖管理碎片化、交叉编译困难、工具链环境不一致及项目迁移成本高等问题。

Cova 参考 Rust Cargo 的统一开发工作流，但不直接复制 Cargo 的底层设计，而是针对 C/C++ 的 ABI、编译器差异、链接模型、系统依赖和跨平台需求建立原生架构。

用户应当能够通过少量命令完成项目创建、依赖安装、编译、运行、测试、格式化、清理和跨平台构建。

```bash
cova new hello
cd hello

cova add fmt
cova build
cova run
cova test
cova fmt
cova clean
```

交叉编译同样应当简单：

```bash
cova build --target aarch64-unknown-linux-musl
```

Cova 自动发现系统工具链，并在缺少兼容工具链、Sysroot 或 Runtime 时，从受信任来源下载缺失组件，安装到 Cova 独立管理的用户目录。

Cova 不应擅自修改系统工具链或系统环境。

## 1.2 产品愿景

让 C/C++ 项目的开发体验接近现代语言生态：

- 项目创建无需手写构建脚本。
- 常规开发不需要直接操作编译器和链接器。
- 依赖声明、版本解析和安装统一管理。
- 交叉编译不再依赖大量手工配置。
- 不同平台拥有一致的命令行体验。
- 构建过程可解释、可追踪、可复现。
- 开发者能够自由选择系统工具链或托管工具链。
- 工具本身使用 C++ 实现，并能够自举。

**长期愿景：成为 C/C++ 生态统一的开发工作流入口。**

## 1.3 产品设计原则

| 原则 | 定义 |
|---|---|
| Cross-Compilation First | 交叉编译是核心能力，不是附加功能 |
| Convention over Configuration | 常见项目零配置，复杂项目显式配置 |
| System First | 默认优先使用符合约束的系统工具链 |
| Managed Fallback | 缺少组件时自动安装 Cova 托管版本 |
| Non-invasive | 不擅自修改系统环境或系统工具链 |
| Reproducibility | 支持锁定依赖、工具链及构建环境 |
| Transparency | 所有自动选择和下载行为均可解释 |
| Extensibility | 编译器、包来源和构建后端可扩展 |
| Self-hosting | Cova 能够构建自身 |
| Security by Default | 下载校验、路径隔离、受信任来源及安全清理 |

---

# 2. 用户群体与应用场景

## 2.1 目标用户

**个人 C/C++ 开发者**

希望快速创建、编译和管理项目，不想维护复杂的构建脚本。

**跨平台应用开发者**

需要在 Linux、Windows、macOS 或其他环境中构建应用程序。

**嵌入式与系统软件开发者**

需要为 ARM、RISC-V 或裸机目标生成代码，并管理特定编译器、Sysroot 和链接脚本。

**开源库维护者**

需要管理依赖、测试矩阵、不同 C++ 标准及不同 ABI 组合。

**CI/CD 与基础设施工程师**

需要稳定、可复现、自动化的跨平台构建环境。

## 2.2 核心用户故事

- US-001：作为开发者，我希望通过一条命令创建一个可运行的 C++ 项目。
- US-002：作为开发者，我希望通过包名添加依赖，而不必手动配置头文件和链接库。
- US-003：作为开发者，我希望自动使用本机已安装且兼容的编译器。
- US-004：作为开发者，当本机缺少目标平台工具链时，我希望 Cova 能自动安装缺失组件。
- US-005：作为跨平台开发者，我希望从 Linux 构建 Windows 或 ARM Linux 程序。
- US-006：作为开发者，我希望指定精确编译器版本并获得一致的选择结果。
- US-007：作为开发者，我希望清理项目构建产物而不删除已下载的依赖和工具链。
- US-008：作为团队成员，我希望使用统一的格式化工具和代码风格。
- US-009：作为 CI 工程师，我希望通过 Lockfile 和工具链指纹验证构建环境。
- US-010：作为系统开发者，我希望知道 Cova 为什么选择某个编译器或重新编译某个文件。
- US-011：作为 Cova 维护者，我希望使用 Cova 构建 Cova 自身。
- US-012：作为库开发者，我希望明确控制依赖的公共头文件、编译选项及链接接口。

---

# 3. 产品范围

## 3.1 核心能力

Cova 分为八个主要功能域：

1. 项目与 Workspace 管理。
2. 构建、运行、测试及清理。
3. 依赖解析、获取及构建。
4. Target 与交叉编译管理。
5. Toolchain Resolver。
6. Sysroot 与 SDK 管理。
7. 格式化、诊断及开发工具集成。
8. 缓存、可复现性与自举。

## 3.2 非目标

以下内容不属于初期交付范围：

- 自行开发 C/C++ 编译器前端。
- 自行开发完整 C++ 标准库。
- 自行实现代码格式化算法。
- 完全替代操作系统的 SDK 管理机制。
- 在所有 Host 上无条件支持所有 Target。
- 第一版即建立完整公共包仓库。
- 第一版即实现分布式构建集群。
- 第一版即保证跨所有编译器的 C++ Modules 兼容。
- 第一版即保证所有构建产物字节级可复现。

---

# 4. CLI 产品设计

## 4.1 命令体系

Cova 使用统一命令行入口：

```bash
cova <command> [options]
```

### 项目管理

| 命令 | 描述 | 优先级 |
|---|---|---|
| `cova new` | 创建新项目 | P0 |
| `cova init` | 初始化现有目录 | P0 |
| `cova metadata` | 输出项目元数据 | P1 |
| `cova workspace` | Workspace 管理 | P1 |

### 构建与开发

| 命令 | 描述 | 优先级 |
|---|---|---|
| `cova build` | 构建项目 | P0 |
| `cova run` | 构建并运行 | P0 |
| `cova clean` | 清理构建产物 | P0 |
| `cova fmt` | 格式化代码 | P0 |
| `cova test` | 构建并执行测试 | P1 |
| `cova check` | 执行不需要最终链接的编译检查 | P1 |
| `cova graph` | 查看构建图 | P2 |
| `cova explain` | 解释构建与缓存决策 | P2 |

### 依赖管理

| 命令 | 描述 | 优先级 |
|---|---|---|
| `cova add` | 添加依赖 | P1 |
| `cova remove` | 移除依赖 | P1 |
| `cova fetch` | 获取依赖源码 | P1 |
| `cova update` | 更新依赖解析结果 | P1 |
| `cova tree` | 查看依赖树 | P1 |
| `cova vendor` | 将依赖源码导出到项目 | P2 |

### 工具链管理

| 命令 | 描述 | 优先级 |
|---|---|---|
| `cova toolchain list` | 列出工具链 | P0 |
| `cova toolchain install` | 安装托管工具链 | P0 |
| `cova toolchain remove` | 卸载托管工具链 | P1 |
| `cova toolchain explain` | 解释工具链选择 | P1 |
| `cova toolchain gc` | 回收未使用的托管组件 | P2 |
| `cova sysroot list` | 列出 Sysroot | P0 |
| `cova sysroot install` | 安装 Sysroot | P0 |
| `cova sysroot verify` | 校验 Sysroot | P1 |
| `cova target list` | 列出支持的 Target | P1 |
| `cova doctor` | 诊断开发环境 | P1 |

### 发布与维护

| 命令 | 描述 | 优先级 |
|---|---|---|
| `cova install` | 安装构建的程序 | P2 |
| `cova package` | 创建发布包 | P2 |
| `cova publish` | 发布包到 Registry | P3 |
| `cova cache gc` | 回收构建缓存 | P2 |

## 4.2 全局参数

```bash
--target <triple>
--profile <name>
--compiler <family@version>
--toolchain <id>
--offline
--locked
--frozen
--no-install
--verbose
--quiet
--jobs <number>
--color <auto|always|never>
```

语义要求：

- `--offline`：禁止网络访问。
- `--locked`：禁止修改现有 Lockfile，缺失或不一致时报错。
- `--frozen`：同时要求离线和锁定状态。
- `--no-install`：禁止安装新组件，但允许使用已有本地资源。
- `--target`：指定产物目标平台。
- `--compiler`：添加编译器家族及版本约束。
- `--toolchain`：指定已知工具链身份。

所有命令必须统一遵守这些参数的适用语义。

---

# 5. 项目初始化

## 5.1 创建项目

```bash
cova new hello
cova new math --lib
cova new engine --workspace
```

默认项目结构：

```text
hello/
├── cova.toml
├── .gitignore
├── .clang-format
├── src/
│   └── main.cpp
└── tests/
```

默认生成一个 C++26 项目。

初始源码应尽可能简单，避免引入非必要依赖。

## 5.2 初始化已有项目

```bash
cova init
```

要求：

- 不覆盖已有文件，除非用户显式确认。
- 检测现有源码目录。
- 识别可执行程序与库项目的常见布局。
- 检测现有构建配置并提供迁移建议。
- 不自动删除 CMake、Meson 等已有配置。

## 5.3 Manifest 格式

Cova 使用 TOML 作为声明式配置格式。

示例：

```toml
[package]
name = "hello"
version = "0.1.0"
edition = "c++26"

[build]
type = "executable"
sources = ["src/**/*.cpp"]

[dependencies]
fmt = "11"

[toolchain]
policy = "system-first"
compiler = "clang"
version = ">=20, <23"
auto-install = true

[profile.dev]
optimization = 0
debug = true

[profile.release]
optimization = 3
lto = true

[format]
tool = "clang-format"
style = "file"
```

上述语法为目标设计草案，最终以 Manifest Schema 规范为准。

## 5.4 配置优先级

建议采用：

1. CLI 显式参数。
2. 项目配置。
3. Workspace 配置。
4. 用户全局配置。
5. Cova 默认配置。

Lockfile 不是普通的配置覆盖层，而是已解析环境的约束记录。Locked 模式下，所有选择必须满足锁定结果。

---

# 6. Target Model

## 6.1 设计目标

Target Model 必须独立于具体编译器。

同一个 Target 可以由多个工具链组合实现。

Target 分为：

- Target Identity。
- Target Constraints。
- Resolved Target Environment。

## 6.2 Target Identity

使用类似 LLVM Target Triple 的标识方式：

```text
x86_64-unknown-linux-gnu
aarch64-unknown-linux-musl
x86_64-pc-windows-gnu
x86_64-pc-windows-msvc
wasm32-wasi
riscv64-unknown-linux-gnu
```

内部必须使用结构化数据模型，而不是在业务逻辑中反复解析字符串。

## 6.3 Target Constraints

支持表达：

- CPU 架构。
- CPU 微架构。
- 指令集扩展。
- OS 和 ABI。
- C Runtime。
- C++ 标准库要求。
- 最低部署系统版本。
- 静态或动态链接策略。
- 异常与 RTTI 要求。
- 线程模型。
- SDK 版本约束。

Target Triple 不应直接包含编译器的具体安装路径。

## 6.4 Target 解析要求

**TGT-001**：能够解析并规范化支持的 Target Triple。

**TGT-002**：未知或不支持的 Target 必须给出明确诊断。

**TGT-003**：能够检测目标平台与 Sysroot 的基本兼容性。

**TGT-004**：支持用户自定义 Target Spec。

**TGT-005**：支持目标相关的依赖和构建配置。

**TGT-006**：能够区分构建平台、工具运行平台和产物目标平台。

**TGT-007**：Target 规范化结果必须具有稳定序列化形式。

---

# 7. Toolchain Resolver

## 7.1 核心目标

根据用户声明的 Target、编译器版本、ABI、标准库及其他约束，解析出完整且经过验证的编译环境。

Cova 不应只寻找编译器路径，而应解析兼容的工具链组件组合。

## 7.2 默认策略

采用：

**System First → Managed Fallback → Trusted Download**

具体规则：

1. 发现系统工具链。
2. 验证系统工具链是否满足约束。
3. 如满足，优先使用。
4. 如不满足，查找 Cova 已管理的工具链。
5. 如仍不满足，从受信任 Registry 获取可安装组件。
6. 验证工具链与 Sysroot、Runtime 的兼容性。
7. 冻结解析结果。
8. 将解析结果交给 Build Graph。

对于精确版本要求，不能擅自选择其他版本。

对于已锁定的精确组件身份，不能仅凭版本号相同而替换为另一份工具链。

## 7.3 策略类型

```toml
[toolchain]
policy = "system-first"
```

支持四种策略：

- `system-first`
- `managed-first`
- `system-only`
- `managed-only`

`system-first` 为默认值。

来源优先级仅在候选满足全部硬性约束之后生效。

## 7.4 混合来源支持

Cova 必须支持不同来源的工具链组件组合。

例如：

```text
Compiler:    /usr/bin/clang++
Linker:      /usr/bin/ld.lld
Sysroot:     ~/.cova/sysroots/aarch64-musl/
Runtime:     ~/.cova/store/...
```

要求：

- 不因缺少 Sysroot 而重复下载已有编译器。
- 不因系统编译器存在就默认认为环境完整。
- 验证编译器、链接器、Runtime 和 Sysroot 的兼容性。
- 记录每个组件的来源与身份。

## 7.5 Toolchain Descriptor

需要包含：

- 工具链 ID。
- 编译器家族和版本。
- 可执行文件路径。
- Host 平台。
- 支持的 Target。
- 编译器资源目录。
- 链接器与归档工具。
- Runtime 信息。
- Sysroot 约束。
- 能力声明。
- 内容哈希。
- 来源及可信度。

## 7.6 能力探测

不能仅通过版本号判断能力。

至少需要探测：

- C/C++ 编译。
- 指定目标架构。
- 目标文件格式。
- 静态库创建。
- 可执行文件链接。
- 标准库头文件可用性。
- C++26 所需语言特性。
- 指定 ABI 与 Runtime。
- 必要的链接选项。

C++ Modules、LTO、Sanitizer 等能力应独立探测。

## 7.7 失败诊断

示例：

```text
error: unable to resolve toolchain

Target:
  aarch64-unknown-linux-musl

Required:
  Clang >= 20
  libc: musl
  C++ standard: C++26

Found:
  System Clang 19
  Managed Clang 18

Missing:
  Compatible compiler
  AArch64 musl sysroot

Suggested action:
  cova toolchain install ...
```

诊断信息必须区分：

- 版本不匹配。
- Target 不支持。
- Sysroot 缺失。
- Runtime 不兼容。
- 下载不可用。
- 离线限制。
- Registry 校验失败。

---

# 8. Sysroot Manager

## 8.1 产品目标

Sysroot Manager 负责发现、安装、校验、组合和管理目标平台的系统编译资源。

它不能只把 Sysroot 当作一个文件夹路径。

## 8.2 管理对象

包括：

- 系统头文件。
- C Runtime。
- C++ Standard Library。
- Compiler Runtime。
- Startup Objects。
- 系统链接库。
- SDK 元数据。
- 平台 ABI 信息。
- 最低部署版本约束。

## 8.3 存储结构

建议使用用户级数据目录：

```text
~/.cova/
├── toolchains/
├── sysroots/
├── store/
├── downloads/
├── registry/
├── cache/
└── config.toml
```

实际路径必须遵循对应操作系统的用户数据目录规范，允许通过配置覆盖。

## 8.4 不可变组件存储

托管组件采用内容寻址或等效的不可变存储机制。

要求：

- 已安装组件不可被静默修改。
- 安装过程具备完整性校验。
- 支持组件复用。
- 支持安全垃圾回收。
- 不影响系统安装的 SDK。
- 不在未授权情况下修改系统目录。

## 8.5 Sysroot 兼容性

必须考虑：

- Target Triple。
- libc 家族及版本。
- ABI。
- CPU 架构。
- Runtime。
- 最低部署系统版本。
- 动态链接器。
- 编译器运行时依赖。

对于 Linux glibc 目标，需要关注符号版本和最低运行环境要求。

## 8.6 自动安装

当系统已有兼容编译器，但缺少 Sysroot 时，只安装缺失组件。

安装必须：

1. 从受信任来源获取元数据。
2. 解析依赖组件。
3. 显示或记录下载计划。
4. 下载到临时目录。
5. 校验内容完整性。
6. 安全解包。
7. 原子提交到托管存储。
8. 执行兼容性验证。
9. 更新安装记录。

对于大型下载、许可要求或交互式使用场景，应提供确认与策略配置。

---

# 9. 构建系统

## 9.1 构建模型

Cova 使用独立的 Build Graph IR。

构建图至少包含以下 Action：

- Compile。
- Scan Dependencies。
- Archive。
- Link。
- Generate。
- Copy。
- Install。
- Test。

Build Graph 不得直接依赖特定编译器命令行格式。

## 9.2 构建后端

初期建议使用 Ninja 作为执行后端。

同时保持独立的构建图模型，为后续原生执行器提供扩展空间。

Cova 不应要求最终用户手动编写 Ninja 文件。

## 9.3 增量构建

要求：

- 追踪源文件变化。
- 追踪头文件依赖。
- 追踪生成文件。
- 追踪编译参数变化。
- 追踪工具链变化。
- 追踪 Sysroot 变化。
- 追踪依赖库变化。
- 避免无必要的重复编译。

增量构建正确性优先于缓存命中率。

## 9.4 并行构建

支持：

```bash
cova build --jobs 8
```

默认根据可用 CPU 和资源限制选择并行度。

构建调度必须尊重依赖图，不能出现尚未生成依赖文件就执行消费者 Action 的情况。

## 9.5 Host Tools 与 Target Artifacts

构建系统必须支持：

- 构建平台执行的代码生成器。
- 目标平台编译的库。
- 跨平台依赖图。
- 生成源码的传递依赖。
- 不同平台工具链共存。

例如 x86-64 Linux 上运行代码生成器，再使用 AArch64 工具链编译生成结果。

## 9.6 C++ Modules

长期目标包括：

- Module Interface Unit 识别。
- Module 依赖扫描。
- BMI 生成。
- 编译顺序计算。
- 编译器特定 BMI 管理。
- Modules 缓存隔离。

第一版不承诺跨编译器 BMI 兼容。

---

# 10. 依赖管理

## 10.1 目标

实现类似 Cargo 的依赖声明与工作流，同时正确处理 C/C++ 的 ABI 和平台差异。

## 10.2 核心命令

```bash
cova add fmt
cova add spdlog@1.15
cova add catch2 --dev

cova remove fmt
cova update
cova fetch
cova tree
```

## 10.3 依赖来源

初期支持：

- 本地路径依赖。
- Git 依赖。
- 受信任 Registry 依赖。

后续支持：

- 系统包。
- CMake Package Config。
- pkg-config。
- vcpkg/Conan 生态适配。

## 10.4 依赖阶段

依赖管理拆分为：

1. Dependency Resolution。
2. Source Fetching。
3. Source Verification。
4. Package Build Planning。
5. Target-specific Compilation。
6. Artifact Caching。

## 10.5 依赖身份

区分：

**Source Identity**

- 包名称。
- 版本。
- 来源。
- 源码哈希。

**Binary Identity**

- Source Identity。
- Target。
- 编译器。
- 标准库。
- ABI。
- Features。
- 构建配置。
- Sysroot / Runtime 约束。

不能因为两个依赖版本相同，就认为二进制构建产物可以共享。

## 10.6 依赖传播

必须支持：

- Public Includes。
- Private Includes。
- Interface Compile Definitions。
- Public Link Dependencies。
- Private Link Dependencies。
- Build Dependencies。
- Development Dependencies。

## 10.7 版本解析

支持语义化版本约束及确定性解析。

需要明确处理：

- 传递依赖冲突。
- Feature 约束。
- 平台条件依赖。
- 可选依赖。
- ABI 相关 Feature。
- 同一包的多版本共存限制。

不能直接照搬 Cargo 的 Feature 合并规则，因为 C++ 的宏和编译选项可能影响 ABI。

## 10.8 Registry 策略

第一版优先构建统一 Package Provider 接口。

不要求立即维护完整公共 Registry。

可以先复用现有包配方和构建生态，但 Cova 自身应拥有独立的 Manifest、解析模型和 Lockfile 语义。

---

# 11. 格式化功能

## 11.1 命令

```bash
cova fmt
cova fmt --check
cova fmt --diff
cova fmt --all
```

## 11.2 格式化后端

默认使用 clang-format。

解析策略：

1. 查找满足要求的系统 clang-format。
2. 查找 Cova 托管的 clang-format。
3. 如果允许，自动下载兼容版本。
4. 执行格式化。

格式化器版本与 C++ 编译器版本独立管理。

## 11.3 文件范围

默认包括项目源码和头文件。

默认排除：

- 构建输出。
- 第三方依赖源码。
- 生成文件。
- Cova 缓存。
- 用户显式忽略的目录。

## 11.4 CI 行为

`cova fmt --check`：

- 不修改文件。
- 格式正确时退出码为 0。
- 格式不正确时返回非零退出码。
- 输出不符合格式要求的文件。
- 支持机器可读的诊断结果。

---

# 12. 清理与缓存

## 12.1 Clean 命令

```bash
cova clean
cova clean --release
cova clean --target aarch64-unknown-linux-musl
cova clean --all
```

默认只清理当前项目的构建产物。

不得默认删除：

- 下载的依赖源码。
- Cova 托管工具链。
- Sysroot。
- 全局共享缓存。

## 12.2 缓存分类

- 项目构建缓存。
- 依赖源码缓存。
- 二进制依赖缓存。
- 工具链组件存储。
- Sysroot 组件存储。
- Registry 元数据缓存。

## 12.3 缓存身份

构建 Action 缓存键必须覆盖：

- Action 类型。
- 完整命令参数。
- 实际输入文件。
- 工具链身份。
- Sysroot 身份。
- Target。
- ABI。
- 构建配置。
- 相关环境变量。

## 12.4 安全清理

要求：

- 仅清理已登记的输出路径。
- 防止路径穿越。
- 防止符号链接导致越界删除。
- 不删除用户未授权的数据。
- 对共享缓存使用引用与可达性判断。
- 支持清理前预览。

---

# 13. 测试与运行

## 13.1 本机运行

```bash
cova run
cova run -- arg1 arg2
```

要求自动构建所需目标，并将应用参数传递给目标程序。

## 13.2 测试

```bash
cova test
cova test --workspace
```

初期支持构建和执行 Cova 声明的测试程序。

后续适配 GoogleTest、Catch2、doctest 等测试框架。

## 13.3 交叉编译 Runner

交叉编译不意味着产物能在构建机上直接运行。

需要支持：

- Native Runner。
- QEMU。
- Wine。
- 模拟器。
- 远程执行设备。

如果 Target 不可在当前环境执行，且未配置 Runner，`cova run` 和 `cova test` 必须明确报错，不得尝试错误地直接运行。

---

# 14. Workspace

## 14.1 Workspace Manifest

```toml
[workspace]
members = [
    "packages/core",
    "packages/network",
    "apps/server"
]
```

## 14.2 功能要求

- 多 Package 构建。
- Workspace 依赖解析。
- 共享 Lockfile。
- 共享工具链策略。
- Package 间依赖。
- Workspace 级构建与测试。
- 单 Package 构建。
- 跨平台依赖图。

支持：

```bash
cova build --workspace
cova test --workspace
cova fmt --workspace
```

---

# 15. Lockfile 与可复现性

## 15.1 锁文件

建议使用：

```text
cova.lock
cova-toolchain.lock
```

前者负责依赖源码解析结果。

后者负责工具链、Sysroot、SDK、Runtime 及相关组件身份。

## 15.2 锁定策略

默认开发模式允许在满足约束时优先使用系统工具链。

Locked 模式必须遵守锁文件约束。

对于系统工具链，应记录其身份、版本、相关组件和可验证指纹。

对于托管工具链，应优先使用内容哈希标识精确资源。

## 15.3 可复现性级别

定义三个级别：

**Level 1：Dependency Reproducibility**

依赖源码和版本一致。

**Level 2：Environment Reproducibility**

工具链、Sysroot、Runtime 和构建参数受到约束。

**Level 3：Artifact Reproducibility**

在受控环境下，构建产物达到可验证的确定性。

Cova 初期以 Level 1 和 Level 2 为目标。

Level 3 作为长期能力，不应在未验证时承诺。

---

# 16. 自举设计

## 16.1 目标

Cova 使用 C++26 实现，并最终使用 Cova 构建自身。

## 16.2 Bootstrap 阶段

**Stage 0**

使用现有 C++ 编译器和最小 Bootstrap 入口构建初始 Cova。

**Stage 1**

使用 Stage 0 生成的 Cova 构建 Cova。

**Stage 2**

使用 Stage 1 生成的 Cova 再次构建，并执行测试与一致性检查。

## 16.3 Bootstrap 约束

- 不依赖完整 Cova Registry。
- 不依赖在线包下载。
- 不依赖尚未构建的 Cova。
- 尽量减少第三方依赖。
- 明确初始编译器版本要求。
- 提供 Linux、Windows 等平台适用的 Bootstrap 方案。

## 16.4 C++26 使用策略

Cova 核心以 C++26 为目标。

但必须区分：

- C++26 语言特性。
- C++26 标准库能力。
- 编译器实验性支持。
- Bootstrap 必需特性。
- 可选优化特性。

不得仅因为编译器接受 `-std=c++26` 就认定其满足全部 Cova 构建要求。

静态反射、Modules 等高级能力应根据实际工具链支持情况分阶段采用。

---

# 17. 软件架构

## 17.1 模块结构

```text
cova/
├── bootstrap/
├── src/
│   ├── core/
│   │   ├── diagnostics/
│   │   ├── filesystem/
│   │   ├── process/
│   │   └── hashing/
│   │
│   ├── manifest/
│   ├── workspace/
│   │
│   ├── platform/
│   │   ├── target/
│   │   ├── triple/
│   │   ├── abi/
│   │   └── constraints/
│   │
│   ├── toolchain/
│   │   ├── discovery/
│   │   ├── resolver/
│   │   ├── registry/
│   │   ├── probe/
│   │   └── providers/
│   │
│   ├── sysroot/
│   │   ├── manager/
│   │   ├── manifest/
│   │   ├── store/
│   │   └── verifier/
│   │
│   ├── package/
│   │   ├── resolver/
│   │   ├── fetcher/
│   │   ├── builder/
│   │   └── lockfile/
│   │
│   ├── build/
│   │   ├── graph/
│   │   ├── action/
│   │   ├── scheduler/
│   │   ├── executor/
│   │   └── cache/
│   │
│   ├── formatter/
│   ├── testing/
│   ├── bootstrap/
│   └── cli/
│
├── tests/
├── examples/
├── docs/
└── cova.toml
```

## 17.2 架构边界

必须保证：

- Target Model 不依赖 CLI。
- Toolchain Resolver 不依赖 Build Executor。
- Sysroot Manager 不依赖具体项目。
- Package Resolver 不直接操作编译器命令行。
- Build Graph 使用统一 Action 模型。
- Compiler Provider 负责生成具体命令。
- CLI 仅负责参数解析、工作流编排及用户交互。

## 17.3 核心数据模型

建议包括：

```cpp
TargetSpec
TargetConstraints

ToolchainDescriptor
ToolchainCapabilities
ResolvedToolchain

SysrootManifest
ResolvedSysroot

CompilationEnvironment

PackageManifest
DependencyGraph
PackageLock

BuildAction
BuildGraph
BuildArtifact
BuildResult
```

其中 `CompilationEnvironment` 必须在进入构建执行阶段前冻结。

---

# 18. 安全需求

## 18.1 下载安全

- 默认只信任明确配置的 Registry。
- 验证下载内容哈希。
- 支持可信签名机制的后续扩展。
- 禁止不安全的归档路径穿越。
- 防止归档文件覆盖托管目录之外的文件。
- 下载失败不得留下可被误认为有效的安装。
- 安装过程应支持原子提交。

## 18.2 构建脚本安全

第三方依赖的构建脚本可能执行任意代码。

Cova 必须明确区分：

- 纯声明式构建。
- 受信任构建配方。
- 执行外部构建脚本。

长期应支持构建沙箱、文件系统访问限制和网络访问限制。

初期不能在没有实际隔离能力时宣称第三方构建完全安全。

## 18.3 系统隔离

- 不修改系统编译器。
- 不覆盖系统 SDK。
- 不自动修改全局 PATH。
- 不使用管理员权限进行普通工具链安装。
- 不在用户未授权时删除系统文件。

---

# 19. 非功能需求

## 19.1 性能

初期目标：

- CLI 冷启动目标低于 100 ms，具体以测试平台为准。
- 小型项目 Manifest 解析目标低于 50 ms。
- 无变化增量构建应避免实际编译和链接。
- 支持多核并行构建。
- 下载缓存应避免重复获取相同内容。

性能指标需要建立固定硬件、操作系统、文件系统和项目基准，不得跨环境直接比较。

## 19.2 稳定性

- 构建失败不得破坏已有有效工具链。
- 下载中断不得留下有效状态标记。
- Lockfile 写入应采用安全替换机制。
- 并发构建不得产生缓存数据竞争。
- 错误的缓存命中属于高优先级正确性缺陷。

## 19.3 可观测性

支持：

```bash
cova build --verbose
cova toolchain explain
cova doctor
cova explain
```

长期支持：

- 结构化 JSON 输出。
- 构建耗时统计。
- 缓存命中率。
- 依赖图导出。
- 构建追踪信息。

## 19.4 IDE 集成

优先支持：

```text
compile_commands.json
```

后续支持：

- VS Code。
- clangd。
- Visual Studio。
- CLion。
- 其他基于 Language Server Protocol 的开发环境。

---

# 20. 平台支持计划

## 20.1 MVP 平台

首个重点验证组合：

**Build Host：Linux x86-64**

**Targets：**

- Linux x86-64 GNU。
- Linux AArch64 musl。

编译器优先使用 Clang/LLVM。

这能够同时验证本机构建、真正交叉编译、Sysroot 解析和混合来源工具链。

## 20.2 后续平台

第二阶段：

- Windows x86-64 GNU。
- WASI wasm32。
- Linux AArch64 GNU。
- Windows 原生构建。

第三阶段：

- Windows MSVC。
- macOS 原生构建。
- Android NDK。
- RISC-V。
- Apple 平台 SDK。
- 嵌入式裸机。

平台支持必须明确区分：

- 可以生成目标代码。
- 可以完成目标链接。
- 可以运行目标程序。
- 可以执行目标测试。
- 可以发布可部署产物。

不能把这五种能力混为一谈。

---

# 21. 版本路线图

## Milestone 0 — Architecture Foundation

**目标：建立可验证的跨平台核心模型。**

交付：

- Target Triple Parser。
- Target Constraints。
- Toolchain Descriptor。
- 系统工具链发现。
- Toolchain Capability Probe。
- Resolved Compilation Environment。
- 结构化诊断系统。

验收标准：

- 能发现系统 Clang/GCC。
- 能识别工具链版本。
- 能判断基本 Target 兼容性。
- 能输出完整解析报告。
- 不依赖实际项目构建即可单独测试 Resolver。

## Milestone 1 — Cova Build MVP

**目标：形成最小可用的本机构建工具。**

交付：

- `cova new`
- `cova init`
- `cova build`
- `cova run`
- `cova clean`
- `cova fmt`
- TOML Manifest。
- Build Graph。
- Ninja 执行后端。
- 基础增量构建。
- clang-format 集成。

验收标准：

- 能创建并构建 C++ 项目。
- 能构建可执行文件和静态库。
- 修改头文件后正确触发重编译。
- 无变化时不执行不必要的编译。
- Clean 不删除全局共享资源。

## Milestone 2 — Cross-Compilation MVP

**目标：实现真正的自动化交叉编译。**

交付：

- Sysroot Manager。
- 托管工具链安装。
- System-first Resolver。
- 混合来源工具链。
- AArch64 Linux musl Target。
- Target-specific 构建输出。
- 工具链完整性校验。
- `--offline` 与 `--no-install`。

验收标准：

- 从 Linux x86-64 构建 AArch64 Linux 程序。
- 系统已有兼容 Clang 时不重复下载。
- 缺少 Sysroot 时能够安装托管 Sysroot。
- 指定不兼容版本时拒绝错误回退。
- 使用目标文件检查工具验证产物架构。
- 在可用 Runner 或目标设备上完成运行验证。

## Milestone 3 — Package Management

**目标：完成 Cargo 式依赖管理闭环。**

交付：

- `cova add`
- `cova remove`
- `cova fetch`
- `cova update`
- `cova tree`
- 本地与 Git 依赖。
- 初始 Registry Provider。
- 依赖 Lockfile。
- Target-specific 依赖构建。
- 依赖二进制缓存。

验收标准：

- 能添加并构建真实第三方 C++ 库。
- 能解析传递依赖。
- 能锁定源码版本。
- 能为 AArch64 重新构建目标依赖。
- 不错误链接 Host 架构库。
- 离线模式能使用已缓存依赖。

## Milestone 4 — Workspace & Self-hosting

**目标：支持真实中型项目及 Cova 自举。**

交付：

- Workspace。
- 测试工作流。
- 多 Package 构建。
- Bootstrap Stage 0。
- Cova 自构建。
- 工具链 Lockfile。
- 更完整的构建诊断。

验收标准：

- Cova 可以构建自身。
- Stage 0 和自举阶段均可重复执行。
- 多 Package 项目可以共享依赖。
- CI 可以使用锁定环境完成构建。

## Milestone 5 — Ecosystem Expansion

**目标：从可用工具发展为跨平台开发基础设施。**

规划：

- Windows/MSVC。
- WASI。
- Android NDK。
- CMake 项目适配。
- 远程构建。
- 远程缓存。
- C++ Modules。
- Package Publishing。
- Registry 服务。
- 高级可复现构建。

---

# 22. 质量保证与测试计划

## 22.1 单元测试

覆盖：

- Target 解析。
- 版本约束。
- Toolchain 候选筛选。
- ABI 兼容性判断。
- Manifest 解析。
- Lockfile 序列化。
- 构建图拓扑排序。
- 缓存键计算。

## 22.2 集成测试

覆盖：

- 系统编译器发现。
- 真实编译与链接。
- Sysroot 安装。
- 依赖获取。
- 跨平台构建。
- 增量构建。
- 格式化。
- Clean 安全性。

## 22.3 故障注入测试

必须模拟：

- 下载中断。
- Registry 不可用。
- 哈希校验失败。
- 编译器缺失。
- 版本不匹配。
- Sysroot 不完整。
- 磁盘空间不足。
- 并发安装同一工具链。
- 缓存损坏。
- 文件系统权限错误。
- 不安全归档文件。
- 无法执行目标平台程序。

## 22.4 交叉编译验收

至少包含：

1. 目标架构验证。
2. 目标 ABI 验证。
3. 链接成功验证。
4. Runtime 依赖检查。
5. Runner 或真实目标设备执行。
6. 第三方依赖交叉编译。
7. Host 代码生成器参与的混合平台构建。

---

# 23. 成功指标

## 23.1 开发体验指标

- 新建简单项目不需要手写构建脚本。
- 常见项目从创建到运行不超过三条核心命令。
- 交叉编译不需要用户手动拼接编译器参数。
- 工具链选择失败时提供明确原因和可执行的修复建议。

## 23.2 正确性指标

- 已覆盖测试中的跨平台构建产物架构正确率为 100%。
- 已覆盖测试中不允许出现错误缓存命中。
- 已覆盖测试中不允许出现 Host/Target 库混用。
- 已覆盖测试中不允许出现越界清理。
- 所有托管下载均经过完整性验证。

## 23.3 性能指标

在固定基准项目与测试环境中追踪：

- CLI 启动时间。
- 完整构建耗时。
- 无变化增量构建耗时。
- 构建图生成耗时。
- 工具链解析耗时。
- 缓存命中率。
- 重复下载避免率。

## 23.4 生态指标

后续追踪：

- 支持的平台组合数量。
- 兼容的第三方包数量。
- 成功构建的真实开源项目数量。
- 工具链安装成功率。
- CI 环境复现成功率。
- CMake 项目迁移成功率。

---

# 24. 风险与应对

| 风险 | 严重性 | 应对措施 |
|---|---|---|
| C++26 编译器支持不完整 | 高 | 建立 Bootstrap 特性基线，逐项探测 |
| 不同编译器 ABI 差异 | 高 | ABI 约束、工具链指纹和兼容性测试 |
| Sysroot 分发与许可限制 | 高 | 可信来源、许可元数据和平台专属 Provider |
| 第三方依赖构建复杂 | 高 | 分阶段支持成熟配方生态 |
| C++ Modules 构建差异 | 高 | 后期独立实现，不阻塞 MVP |
| 构建缓存错误复用 | 高 | 完整输入追踪和保守缓存策略 |
| 系统工具链变化 | 中高 | 指纹检查及 Locked 模式 |
| 自动下载供应链风险 | 高 | 内容校验、来源限制、安全解包 |
| Windows/MSVC 复杂性 | 中高 | 独立 Provider 和分阶段支持 |
| 开发范围过大 | 高 | 严格按 Milestone 验收 |
| 自举依赖循环 | 高 | 最小 Stage 0 和独立 Bootstrap |
| 与现有工具竞争 | 中高 | 聚焦交叉编译、零配置和环境管理 |

---

# 25. 开放设计问题

以下内容需要在正式架构冻结前确定。

**OD-001：构建执行后端**

第一版是否固定 Ninja，还是同步开发原生 Executor？

建议：先使用 Ninja，保留独立 Build Graph IR。

**OD-002：工具链自动下载**

自动下载是否需要交互确认？

建议：交互环境可按下载规模和信任策略确认；CI 提供显式允许或禁止安装的配置。

**OD-003：依赖 Registry**

是否立即建设自有公共 Registry？

建议：先实现 Provider 接口和锁文件，再决定 Registry 的维护范围。

**OD-004：CMake 兼容**

兼容层应如何映射 CMake Target 的 Includes、Compile Definitions 和 Link Dependencies？

建议：独立设计适配层，不让 CMake 概念污染 Cova 核心 Build IR。

**OD-005：工具链锁定**

如何平衡 System-first 默认策略与严格环境复现？

建议：区分 Flexible 和 Locked 模式，明确系统工具链身份验证规则。

**OD-006：C++26 特性基线**

哪些 C++26 特性是构建 Cova 的硬性要求？

建议：通过编译器能力矩阵定义，不仅依赖编译器版本号。

**OD-007：远程执行模型**

未来如何表达执行平台与目标平台的关系？

建议：从 MVP 开始在 Build Action 中保留独立的平台字段。

---

# 26. 产品发布标准

## 26.1 Alpha

必须具备：

- 项目初始化。
- Manifest 解析。
- 本机 C++ 构建。
- 基础增量构建。
- Clean。
- Format。
- 系统工具链发现。

允许接口和 Manifest Schema 调整。

## 26.2 Beta

必须具备：

- 可验证的交叉编译。
- Sysroot Manager。
- 托管工具链安装。
- System-first 策略。
- 基础依赖管理。
- Lockfile。
- 基础 Workspace。
- CI 集成测试。

Beta 应开始提供 Manifest 和 CLI 兼容性承诺。

## 26.3 Stable 1.0

建议至少满足：

- 已声明支持的平台组合通过完整 CI。
- 真实项目可以完成依赖安装、构建和测试。
- Cova 可以自举。
- Toolchain Resolver 具有完整错误诊断。
- 工具链与 Sysroot 安装具备完整性保障。
- 增量构建正确性经过系统验证。
- Lockfile 语义稳定。
- 主要 CLI 和 Manifest Schema 稳定。
- 具备用户文档、迁移指南及版本兼容政策。

---

# 27. 最终产品定义

**Cova 是一个以真正交叉编译为核心、使用 C++26 实现、能够自举的现代 C/C++ 构建与开发环境管理系统。**

它提供 Cargo 式的项目工作流，但针对 C/C++ 的工具链、ABI、Sysroot、标准库及多平台构建需求进行专门设计。

Cova 的默认行为是：

1. 优先使用满足约束的系统环境。
2. 系统环境不满足时，优先复用 Cova 已托管的组件。
3. 仍有缺失时，从受信任来源安装所需组件。
4. 不擅自修改系统工具链或 SDK。
5. 通过独立的 Target Model、Toolchain Resolver 和 Sysroot Manager 生成完整编译环境。
6. 使用统一 Build Graph 执行本机构建和交叉编译。
7. 通过 Manifest、Lockfile 和内容校验管理依赖与构建环境。
8. 通过统一 CLI 提供创建、构建、运行、测试、格式化、清理和发布工作流。

**核心价值主张：**

*One project. One workflow. Multiple platforms.*

一个项目，一套工作流，面向多个目标平台。

---

**文档结束 — Cova PRD v1.0**

