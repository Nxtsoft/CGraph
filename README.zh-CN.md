<div align="center">

<img src="assets/hero.svg" alt="CGraph — 让你的代码库成为可查询的知识图谱" width="100%">

[English](README.md) · **简体中文**

[![License: MIT](https://img.shields.io/badge/License-MIT-6ea8fe?style=flat-square)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus&logoColor=white)](CMakeLists.txt)
[![CMake](https://img.shields.io/badge/CMake-vcpkg-064F8C?style=flat-square&logo=cmake&logoColor=white)](CMakePresets.json)
[![MCP](https://img.shields.io/badge/MCP-2024--11--05-59d499?style=flat-square)](https://modelcontextprotocol.io)
[![PRs welcome](https://img.shields.io/badge/PRs-欢迎贡献-f5c451?style=flat-square)](#贡献)

*扫描项目 · 将代码结构提取为确定性图谱 · 通过守护进程、瘦客户端或 MCP 服务器在 ~10 毫秒内查询。*

</div>

## 目录

[为什么用 CGraph？](#为什么用-cgraph) ·
[架构](#架构) ·
[仓库结构](#仓库结构) ·
[产出效果](#产出效果) ·
[输出格式](#输出格式) ·
[性能](#性能) ·
[支持的语言](#支持的语言) ·
[快速开始](#快速开始) ·
[安装与配置](#安装与配置) ·
[与编码代理配合使用](#与编码代理配合使用) ·
[命令行工具](#命令行工具) ·
[守护进程与瘦客户端](#守护进程与瘦客户端) ·
[MCP 服务器](#mcp-服务器) ·
[宿主集成](#宿主集成) ·
[语义增强](#语义增强) ·
[开发须知](#开发须知) ·
[贡献](#贡献) ·
[许可证](#许可证)

## 为什么用 CGraph？

要回答**“谁调用了这个？”**或**“改动它会影响什么？”**，通常得翻阅几十个文件——而答案并不在任何单个文件里，它藏在文件*之间的关系*中。CGraph 预先算好这些关系，让你（和你的 AI 代理）用**图谱导航，而非盲目 grep**。

| | |
| --- | --- |
| 🔗 **反向依赖与影响范围** | `graph_impact` 在 ~10 毫秒内返回某符号的所有传递依赖者。在一个真实的 10,706 节点仓库中，它瞬间找出某个“上帝文件”的 **93 个依赖者**——这是 grep 给不了的答案。 |
| 🧭 **用图谱导航，不再 grep** | 按中心度排序的搜索、节点邻域、最短路径，以及**受 token 预算约束的源码打包**——warm 状态下 ~10 毫秒返回，省去大量消耗上下文的文件读取。 |
| 🏛️ **一眼看清架构** | 中心度排序凸显承载全局的关键文件；Leiden/Louvain 社区检测自动将仓库聚成模块。 |
| ⚡ **确定性且快速** | 完整的 **1 万节点图谱约 2 秒**构建完成；提取路径中没有任何 LLM——相同输入，永远得到相同图谱。 |
| 🤖 **为编码代理而生** | 标准 **MCP** 服务器将图谱直接接入 Claude / Codex，让它们基于结构推理，而不是盲目读文件。 |

> **一句话概括：** 对于大到无法装进脑子的代码库，CGraph 就是那个帮你决定*该读哪段代码*的索引——预先算好、按中心度排序、并通过 MCP 暴露的 `ctags`／“查找所有引用”。

## 架构

<div align="center"><img src="assets/architecture.svg" alt="CGraph 架构" width="100%"></div>

- **`cgraph`** —— 一次性扫描 → 可移植的磁盘导出（`graph.json`、`graph.html`、`graph.svg`、`obsidian.md`、`cypher.txt`、`call-flow.html`）。
- **`graphd` + `cgraph-client`** —— 常驻的按项目守护进程，实时监听文件变化；warm 状态下 `query`／`path`／`explain`／`impact`／`context` 均为 ~10 毫秒。
- **`cgraph-mcp`** —— Model Context Protocol 服务器，让代理直接在图谱上导航。

引擎负责确定性提取、片段校验、缓存状态与本地图谱变更；宿主负责模型选择与语义增强。

## 仓库结构

```text
src/cli/          一次性 CLI 入口：cgraph
src/daemon/       守护进程入口：graphd
src/client/       瘦客户端运行时与 cgraph-client 可执行文件
src/mcp/          MCP 请求处理与 cgraph-mcp 可执行文件
src/engine/       检测、提取、图谱构建、分析、守护进程操作
tests/smoke/      引擎、守护进程、MCP 与集成路径的 CTest 冒烟测试
tests/fuzz/       可选的 libFuzzer 目标
integrations/     宿主 hook 与 always-on 集成脚本
docs/             宿主集成契约与基准说明
vendor/           内置的 tree-sitter core 与语法
```

## 产出效果

一次扫描即可将源码树变成可交互、可探索的图谱——社区着色，枢纽节点按中心度调整大小：

<div align="center"><img src="assets/graph-example.png" alt="CGraph 在 10,708 节点代码库上的交互视图" width="90%"></div>

<sub>真实的 `graph.html` 视图，作用于一个 10,708 节点／28,945 条边的代码库——自包含 HTML，无任何外部 JS。</sub>

## 输出格式

- `graph.json` —— 带图元数据、节点与边的有向 node-link JSON
- `graph.html` —— 浏览器可读的交互式图谱视图
- `graph.svg` —— 静态图谱可视化
- `obsidian.md` —— 面向 Obsidian 式导航的 markdown 导出
- `cypher.txt` —— Neo4j Cypher 语句
- `call-flow.html` —— 浏览器可读的调用流视图

## 性能

交互视图过去每次打开都在浏览器里跑一遍 O(N²) 力导向模拟。现在布局**在 C++ 侧用 igraph 预先算好一次**，重绘也做了视口裁剪——两者都**零依赖**：

<div align="center"><img src="assets/performance.svg" alt="稳定耗时：35 秒 → 0 毫秒" width="100%"></div>

| 图规模 | **优化前**稳定耗时 | **优化后** | 单帧重绘 |
| --- | --- | --- | --- |
| 10,000 节点 | **35.2 秒**（主线程卡死） | **~0 毫秒**（静态布局） | **降低 20–34 倍**（缩放/平移） |

## 支持的语言

基于 tree-sitter 的结构提取，另有若干配置格式的正则/结构化提取：

<p>
<img src="https://img.shields.io/badge/C-A8B9CC?style=for-the-badge&logo=c&logoColor=black" alt="C">
<img src="https://img.shields.io/badge/C++-00599C?style=for-the-badge&logo=cplusplus&logoColor=white" alt="C++">
<img src="https://img.shields.io/badge/Go-00ADD8?style=for-the-badge&logo=go&logoColor=white" alt="Go">
<img src="https://img.shields.io/badge/Groovy-4298B8?style=for-the-badge&logo=apachegroovy&logoColor=white" alt="Groovy">
<img src="https://img.shields.io/badge/Java-007396?style=for-the-badge&logo=openjdk&logoColor=white" alt="Java">
<img src="https://img.shields.io/badge/JavaScript-F7DF1E?style=for-the-badge&logo=javascript&logoColor=black" alt="JavaScript">
<img src="https://img.shields.io/badge/Kotlin-7F52FF?style=for-the-badge&logo=kotlin&logoColor=white" alt="Kotlin">
<img src="https://img.shields.io/badge/Python-3776AB?style=for-the-badge&logo=python&logoColor=white" alt="Python">
<img src="https://img.shields.io/badge/Ruby-CC342D?style=for-the-badge&logo=ruby&logoColor=white" alt="Ruby">
<img src="https://img.shields.io/badge/Scala-DC322F?style=for-the-badge&logo=scala&logoColor=white" alt="Scala">
<img src="https://img.shields.io/badge/TypeScript-3178C6?style=for-the-badge&logo=typescript&logoColor=white" alt="TypeScript">
<img src="https://img.shields.io/badge/TSX-61DAFB?style=for-the-badge&logo=react&logoColor=black" alt="TSX">
</p>

另外还支持对 Apex、Delphi 表单/源码、MSBuild/XML 工程文件以及 MCP 配置文件的结构化/正则提取。

## 快速开始

**前置条件：** CMake 3.25+、Ninja、C++20 编译器、Git、Fortran 编译器（`gfortran`——igraph 会引入 `lapack-reference`），以及 vcpkg。完整步骤见 [安装与配置](#安装与配置)。

```sh
git clone --recurse-submodules https://github.com/taylor009/CGraph.git && cd CGraph
git clone https://github.com/microsoft/vcpkg .vcpkg && ./.vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT="$PWD/.vcpkg"
cmake --preset default && cmake --build --preset default

# 为本仓库构建图谱，然后打开交互视图
build/default/src/cli/cgraph --root . --out cgraph-out
open cgraph-out/graph.html
```

## 安装与配置

> **状态：** 早期原生实现。完整命令面（CLI、守护进程、瘦客户端、MCP 服务器）均已具备并经过测试；尚无打包发布——需用 CMake + vcpkg 从源码构建，并从构建目录运行二进制（或将其软链接到 `PATH`）。

### 前置条件

- CMake 3.25 及以上
- Ninja
- C++20 编译器（较新的 Clang 或 GCC；Xcode 命令行工具自带的 Apple Clang 亦可）
- Fortran 编译器（如 `gfortran`）—— igraph 的 vcpkg 构建会引入 `lapack-reference`，需要它（`sudo apt-get install -y gfortran` / `brew install gcc`）
- Git
- vcpkg（本地副本即可——见第 2 步）。`curl`、`igraph`、`nlohmann-json`、`utf8proc` 在 `vcpkg.json` 中声明，首次配置时构建。`tree-sitter` 内置于 `vendor/tree-sitter`。

### 1. 克隆（含子模块）

```sh
git clone --recurse-submodules https://github.com/taylor009/CGraph.git && cd CGraph
# 已经克隆但没带子模块？
git submodule update --init --recursive
```

### 2. 让 CMake 指向 vcpkg

```sh
git clone https://github.com/microsoft/vcpkg .vcpkg   # 完整深度——浅克隆会缺失锁定的 baseline
./.vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT="$PWD/.vcpkg"
```

### 3. 配置、构建、验证

```sh
cmake --preset default
cmake --build --preset default            # 首次构建会编译 vcpkg 依赖——需数分钟
ctest --preset default                    # 冒烟测试
build/default/src/cli/cgraph --root . --out cgraph-out
```

二进制位于 `build/default/src/{cli/cgraph, daemon/graphd, client/cgraph-client, mcp/cgraph-mcp}`。

### 4.（可选）将二进制加入 PATH

```sh
mkdir -p ~/.local/bin
for b in cli/cgraph daemon/graphd client/cgraph-client mcp/cgraph-mcp; do
  ln -sf "$PWD/build/default/src/$b" ~/.local/bin/
done
```

> 下方的 MCP 客户端配置仍应使用二进制的绝对路径，因为客户端未必继承你交互式 shell 的 `PATH`。

### 开发构建

```sh
cmake --preset sanitizers && cmake --build --preset sanitizers && ctest --preset sanitizers  # ASan/UBSan
cmake --preset fuzzers    && cmake --build --preset fuzzers    && ctest --preset fuzzers      # libFuzzer
```

fuzzer 预设需要带 libFuzzer 运行时的 Clang 工具链；若 Apple 命令行工具缺失，请改用上游 LLVM/Clang。

## 与编码代理配合使用

`cgraph-mcp` 是基于 stdio 的标准 [MCP](https://modelcontextprotocol.io) 服务器（协议 `2024-11-05`）。注册一次，代理即可通过快速图谱查询导航代码库，而非盲目 grep/读文件：

| 工具 | 用途 |
| --- | --- |
| `graph_query` | 按文本搜索节点；按中心度排序 |
| `graph_explain` | 某节点的邻域（调用者、被调用者、导入） |
| `graph_impact` | 改动某节点的传递影响范围 |
| `graph_path` | 两个节点之间的最短路径 |
| `graph_context` | 受 token 预算约束的源码打包（支持自适应聚合） |
| `graph_update` | 内容校验式同步；返回 `content_root` 用于锁定后续读取 |
| `graph_status` | 守护进程、图谱与增强状态 |
| `graph_remember` / `graph_recall` | 会话记忆——`/compact` 前存档，之后恢复 |
| `graph_shutdown` | 停止守护进程 |

`graph_context` 有两种聚合模式。默认（`gather: "fixed"`）打包整个 k 跳邻域。带上任务查询时，`gather: "adaptive"` 保留完整的 2 跳核心，仅沿与查询相关的节点扩展第三跳——在检索评测中，它以 **+13%** 的候选 token 换来 grade-2 召回率 **+0.057**，而完整 3 跳需要多付 **+96%**（需提供 `query`/`q`）。

服务器按 `--root`、`CLAUDE_PROJECT_DIR`、当前工作目录的顺序解析项目根，并自动定位 `graphd`（显式 `--daemon` 优先，其次 `CGRAPH_DAEMON_PATH`，再次与 `cgraph-mcp` 相邻的 `graphd`）。首次调用触发一次性构建（数秒）；期间结果带 `"graph_state": "building"`，因此空结果绝不会被误当作“无匹配”。后续查询为 warm（~10 毫秒）。下方示例中，请将 `/abs/path/to/CGraph` 替换为本仓库的绝对路径。

### Claude Code

Claude Code 会按会话设置 `CLAUDE_PROJECT_DIR`，因此一次注册即可跨所有项目使用：

```sh
claude mcp add --scope user --transport stdio cgraph \
  -- /abs/path/to/CGraph/build/default/src/mcp/cgraph-mcp \
     --daemon /abs/path/to/CGraph/build/default/src/daemon/graphd
```

或在仓库根目录提交一个按项目作用域的 `.mcp.json`，以便与协作者共享：

```json
{
  "mcpServers": {
    "cgraph": {
      "command": "/abs/path/to/CGraph/build/default/src/mcp/cgraph-mcp",
      "args": ["--daemon", "/abs/path/to/CGraph/build/default/src/daemon/graphd"]
    }
  }
}
```

在 Claude Code 中用 `/mcp` 验证。本仓库还在 `integrations/skills/` 下提供宿主技能——`cgraph`（遇到结构性问题优先用图谱工具）与 `cgraph-enrich`（语义增强流程）。用 `cgraph skills install` 安装；用 `cgraph drain install` 添加受状态门控的定时增强 drainer。

### Codex CLI

Codex 不设置 `CLAUDE_PROJECT_DIR`，因此服务器回退到 Codex 启动它的工作目录：

```sh
codex mcp add cgraph \
  -- /abs/path/to/CGraph/build/default/src/mcp/cgraph-mcp \
     --daemon /abs/path/to/CGraph/build/default/src/daemon/graphd
```

……或直接编辑 `~/.codex/config.toml`（在 `args` 中加入 `"--root", "/abs/path/to/your/project"` 可无视工作目录锁定某个项目）：

```toml
[mcp_servers.cgraph]
command = "/abs/path/to/CGraph/build/default/src/mcp/cgraph-mcp"
args = ["--daemon", "/abs/path/to/CGraph/build/default/src/daemon/graphd"]
```

编辑后重启 Codex，在 TUI 中运行 `/mcp` 确认。

### Cursor、Windsurf 及其他 MCP 客户端

任何能启动 stdio 命令的 MCP 客户端都可用——添加带 command 与 args 的服务器条目（多数使用与 Claude Code `.mcp.json` 类似的 `mcpServers` JSON 块）。对不设置 `CLAUDE_PROJECT_DIR` 的客户端请显式设置 `--root`。也可手动冒烟测试：

```sh
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  | build/default/src/mcp/cgraph-mcp --root . \
      --daemon build/default/src/daemon/graphd
```

## 命令行工具

```sh
cgraph [--root PATH] [--out PATH]
cgraph enrich-plan [--root PATH] [--out PATH] [--drop DIR]
cgraph enrich-ingest [--root PATH] [--out PATH] [--drop DIR]
```

默认值：`--root .`、`--out cgraph-out`、`--drop` → 输出路径下 CGraph 的语义 drop 目录。

```sh
# 构建确定性导出。
build/default/src/cli/cgraph --root /path/to/project --out /tmp/cgraph-out
# 为宿主增强创建语义分块计划。
build/default/src/cli/cgraph enrich-plan --root /path/to/project --out /tmp/cgraph-out
# 摄取宿主写入的 chunk_NN.json 片段并重新导出图谱。
build/default/src/cli/cgraph enrich-ingest --root /path/to/project --out /tmp/cgraph-out
```

## 守护进程与瘦客户端

```sh
build/default/src/daemon/graphd --root /path/to/project
```

守护进程运行期间监听项目树：源码改动会在数秒内增量并入图谱（大批量改动，如切换分支，会合并为一次完整重扫），增量状态在后台及关闭时重新持久化到 `cgraph-out/`。`--no-watch` 可禁用。

可选守护进程参数：

```sh
graphd --root PATH --idle-timeout SECONDS --no-watch
graphd --benchmark-query --graph PATH --query TEXT
graphd --version
```

使用瘦客户端（响应为 JSON；`status` 含进程元数据、节点/边计数、缓存命中率与增强状态）：

```sh
build/default/src/client/cgraph-client --root /path/to/project status
build/default/src/client/cgraph-client --root /path/to/project query '{"q":"Parser"}'
build/default/src/client/cgraph-client --root /path/to/project explain '{"id":"Parser"}'
build/default/src/client/cgraph-client --root /path/to/project path '{"source":"A","target":"B"}'
build/default/src/client/cgraph-client --root /path/to/project update '{"path":"."}'
build/default/src/client/cgraph-client --root /path/to/project shutdown
```

## MCP 服务器

`cgraph-mcp` 通过 stdio 讲 MCP：换行分隔的 JSON-RPC 2.0，实现 `initialize`、`tools/list`、`tools/call` 与 `notifications/initialized`（协议 `2024-11-05`）。工具调用经由与瘦客户端相同的守护进程操作处理器；无效 JSON 会收到 JSON-RPC 解析错误。注册方式与工具列表见 [与编码代理配合使用](#与编码代理配合使用)。

## 宿主集成

CGraph 将供应商与模型相关的关注点保留在原生二进制之外。宿主集成使用 `cgraph-client` 进行图谱操作，并通过自身的代理/模型流程分派语义工作。参考 hook 接受确定性守护进程操作：

```sh
integrations/hooks/cgraph-hook.sh status
integrations/hooks/cgraph-hook.sh query '{"q":"GraphSnapshot"}'
```

常用环境变量：`CGRAPH_CLIENT`（客户端可执行文件）、`CGRAPH_PROJECT_ROOT`（项目根）、`CGRAPH_DAEMON`（守护进程路径）、`CGRAPH_INTERVAL_SECONDS`（always-on 间隔，默认 `30`）、`CGRAPH_REFRESH_ON_START`（设为 `0` 跳过初次更新）、`CGRAPH_ONCE`（设为 `1` 只做一次状态检查后退出）。运行 always-on 参考循环：

```sh
CGRAPH_CLIENT=build/default/src/client/cgraph-client \
CGRAPH_PROJECT_ROOT=/path/to/project \
integrations/always-on/cgraph-always-on.sh
```

完整宿主契约见 `docs/host-skill-contract.md`。

## 语义增强

宿主驱动的流程：(1) CGraph 为未缓存或过期的语义输入生成分块计划；(2) 宿主用自己的模型/代理处理每个分块；(3) 宿主为每个完成的分块向语义 drop 目录写入恰好一个 `chunk_NN.json` 片段；(4) CGraph 在图谱变更前校验每个片段；(5) 合法片段更新图谱与语义缓存，非法片段被拒绝且不改动快照。

片段采用如下 node-link 结构（必填：节点 `id`/`label`；边 `source`/`target`/`relation`；超边 `id`/`nodes`/`relation`。可选：`source_file`、`source_location`、`type`/`kind`、`confidence`、`confidence_score`、`properties`、`warnings`）：

```json
{
  "nodes": [{ "id": "doc:architecture", "label": "Architecture", "kind": "document" }],
  "edges": [{ "source": "doc:architecture", "target": "component:engine", "relation": "describes" }],
  "hyperedges": []
}
```

## 开发须知

- 引擎中的提取行为保持确定性。供应商相关逻辑归入宿主集成。
- 在 `tests/smoke/` 下为引擎行为与集成面添加冒烟测试。
- 在 `tests/fuzz/` 下为解析器或提取器加固添加 fuzzer 覆盖。
- 优先扩展中央语言配置与提取流水线，而非在消费方添加临时提取逻辑。
- 交互视图刻意保持**零依赖**——生成的 HTML 中不引入任何 JS 库。

## 贡献

欢迎提交 Issue 与 PR。请使用 `default` 预设构建，保持 `ctest --preset default` 通过，迭代时优先用 `sanitizers` 预设。

## 许可证

[MIT](LICENSE)。
