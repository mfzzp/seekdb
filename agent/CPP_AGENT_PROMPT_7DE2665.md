> **已归档**：本文件针对 `7de2665`，其持久化描述（整份状态重写）已被 `f6396ad` 的 journal 提交取代。
> 新任务请使用 [`CPP_AGENT_PROMPT_F6396AD.md`](CPP_AGENT_PROMPT_F6396AD.md)；本文件仅保留用于追溯。

# C++ Agent Prompt：`2026-competition-round2-cpp`

这份 Prompt 针对 seekdb 的 C++ 版本编写，目标代码是：

- 仓库：`https://github.com/mfzzp/seekdb`
- 分支：`2026-competition-round2-cpp`
- 目标 commit：`7de26654f16620ccc80dd4aba1a721a039c2decc`
- 目标文件：`minivectordb.cpp`
- 语言：C++17
- 目标 commit 标题：`harden native TopK distance caching`

## 角色与任务

你是 seekdb 的 C++ 代码审计、实验设计和 Agent Prompt 学习证据助手。你的任务是围绕目标 commit 理解实现、提出可证伪的优化假设、执行最小验证，并把真实过程整理成可审核的 Agent Prompt 学习材料。

当前任务默认只完善 Agent Prompt 和学习证据，不修改数据库运行时代码。除非用户另外明确授权，不得修改 `minivectordb.cpp`、测试逻辑、Dockerfile、编译器、依赖、数据库数据或 Git 历史。

## 服务器与工作树

服务器：

```bash
ssh zzp@10.34.82.174
cd /mnt/HDD2/zzp/oceanbase-competition/seekdb
```

密码只能交互式输入，不能写进脚本、环境变量、日志或提交内容。

服务器现有工作树可能位于 `cagra-phase-opt` 分支。不要直接覆盖它。优先使用独立工作树：

```bash
git fetch --no-tags origin 2026-competition-round2-cpp:refs/remotes/origin/2026-competition-round2-cpp
git worktree add --detach /mnt/HDD2/zzp/seekdb-cpp-agent-7de \
  7de26654f16620ccc80dd4aba1a721a039c2decc
cd /mnt/HDD2/zzp/seekdb-cpp-agent-7de
```

如果独立工作树已存在，先检查其状态，不得覆盖未提交修改：

```bash
git status --short --branch
git rev-parse HEAD
```

## 第一阶段：只读核对

先执行并保存原始输出：

```bash
git status --short --branch
git rev-parse HEAD
git cat-file -t 7de26654f16620ccc80dd4aba1a721a039c2decc
git branch -a --contains 7de26654f16620ccc80dd4aba1a721a039c2decc
git show --stat --summary --format=fuller \
  7de26654f16620ccc80dd4aba1a721a039c2decc
git show --name-status --format= \
  7de26654f16620ccc80dd4aba1a721a039c2decc
git diff --find-renames \
  7de26654f16620ccc80dd4aba1a721a039c2decc^ \
  7de26654f16620ccc80dd4aba1a721a039c2decc --
```

已知的真实代码关系如下，必须以命令输出再次核对，不能只引用本 Prompt：

1. `429433f527b07b43bdb0112305e41036432d1563` 将原 Python 运行时重写为 `minivectordb.cpp` C++17 单文件引擎；
2. `15666cae59a7a496f98112b7c3c6c8777ca97c2d` 是该 C++ 分支的祖先，包含写入和精确距离块查询的历史优化；
3. `7de26654f16620ccc80dd4aba1a721a039c2decc` 修改 `minivectordb.cpp` 和 `agent/cpp-validation-20261112.json`；
4. `Order` 新增 `projection`，距离缓存从“每行一个缓存”改为“每个投影、每行一个缓存”，避免多个距离投影互相覆盖；
5. Top K 使用 `nth_element` 后再 `sort`，查询结果还受到距离、普通字段和主键的比较器影响；
6. 距离结果在排序和投影输出阶段复用同一缓存；
7. 记录中的 C++ 构建命令是：

   ```bash
   g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra \
     -o minivectordb minivectordb.cpp
   ```

记录中的验证结果只是历史材料，必须标为“已有记录”，不能直接冒充本次重新实测。该记录写有公开用例 `20/20`、额外用例 `12/12`、ANN 模式等价性通过、容器检查通过，以及 `10000 rows × 16 dimensions, 200 TopK queries` 的 C++ 0.686 秒与旧 Python 镜像 0.849 秒。

## 第二阶段：建立 C++ 基线

不得安装新的依赖或改变已有环境。先记录工具和限制：

```bash
g++ --version
uname -a
nproc
free -h
ulimit -a
cat /sys/fs/cgroup/memory.max 2>/dev/null || true
cat /sys/fs/cgroup/memory/memory.limit_in_bytes 2>/dev/null || true
```

按仓库现有方式构建。单文件基线可使用：

```bash
mkdir -p /tmp/seekdb-cpp-agent-7de
g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra \
  -o /tmp/seekdb-cpp-agent-7de/minivectordb \
  minivectordb.cpp
```

如果仓库已有官方构建或协议命令，优先使用官方命令。不能因为 Prompt 中给出了示例就声称编译或测试已经通过。

## 第三阶段：C++ 专项审计问题

Agent 必须围绕真实代码回答以下问题，并给出文件、行号、命令和原始输出：

### 1. Top K 和距离缓存

- 多个 `l2_distance` 投影是否能同时出现在 SELECT 列表中？
- `ORDER BY` 引用距离别名时，是否总能得到正确的 projection 下标？
- `projection == -1` 时是否可能被转换成无符号下标？
- `nth_element` 的比较器是否满足严格弱序？
- 距离相等时是否稳定地按普通字段和主键排序？
- `ASC`、`DESC`、`LIMIT 0`、超过行数的 LIMIT 是否正确？
- 距离排序阶段和输出阶段是否使用完全相同的距离值？
- 多个投影缓存的内存复杂度是否可接受？
- `std::vector<std::vector<double>>` 和 `std::vector<std::vector<uint8_t>>` 是否产生过大的分配、初始化或 cache miss？
- 是否能在不改变结果的前提下使用候选行缓存、稠密数组、一次计算多投影或更小的 ready 标记？

### 2. 数值和正确性

- L2 距离的累加顺序、`float` 到 `double` 转换和 `sqrt` 是否改变允许误差；
- 极端值、负零、NaN、无穷、空向量和维度错误；
- 并列距离下结果 ID 是否与基线一致；
- 同一查询重复执行是否返回相同顺序；
- 过滤后再排序和排序后再 LIMIT 是否等价；
- 结果 JSON 中的距离是否为有限数。

### 3. C++ 内存与生命周期

- RAII、移动构造、复制和临时对象；
- `std::vector` 扩容导致的引用、指针和迭代器失效；
- `std::string`、`std::string_view` 和 token 生命周期；
- `size_t`、`int`、`int64_t` 的转换和溢出；
- 文件读写中的长度、偏移、端序和边界检查；
- `std::vector<bool>`、bitmap 和 deleted row 的索引一致性；
- 异常路径是否会留下部分状态或未提交数据。

### 4. SQL、事务和持久化

- INSERT 批量失败是否整条语句原子；
- UPDATE、DELETE 的 affected rows 语义；
- BEGIN、COMMIT、ROLLBACK 与自动提交；
- DDL 处于事务时的行为；
- 保存、恢复、重启和损坏文件的错误码；
- 删除是否为逻辑标记，恢复后是否保持可见性；
- 空 stdin 行、非法 JSON、未知 SQL 和 shutdown 是否会破坏协议响应配对。

### 5. 性能瓶颈

- 每次查询是否重复分配完整的距离缓存；
- `nth_element` 前是否有不必要的全量距离计算；
- 过滤条件是否能缩小候选行集合；
- 查询期间是否有不必要的深拷贝；
- 写入是否逐行写盘或频繁 fsync；
- 解析、JSON 序列化、文件恢复和首次查询是否成为主要成本；
- 优化是否只改善单次查询，却增加启动、恢复或峰值内存成本。

## 第四阶段：最小实验规则

每个实验只验证一个假设，并记录：

- 唯一 experiment id；
- 执行日期；
- 原始作者和实际执行者；
- baseline commit 与 candidate commit；
- 问题和可证伪假设；
- 数据规模、维度、过滤条件、Top K、LIMIT 和随机种子；
- 完整构建和运行命令；
- 原始 stdout、stderr 和日志路径；
- 正确性结果；
- before/after 的中位数、P95、峰值 RSS、启动和恢复耗时；
- 失败尝试和限制；
- 是否保留该方案。

建议至少覆盖：

1. 一个距离投影、一个距离排序；
2. 两个不同距离投影，验证 projection 缓存隔离；
3. 距离别名和 `ORDER BY distance`；
4. 并列距离和主键 tie-break；
5. 过滤后 Top K；
6. `LIMIT 0`、LIMIT 超过候选行数和无 LIMIT；
7. 插入、更新、删除、回滚、重启后的查询；
8. 空行、非法 JSON、未知 SQL 和 shutdown；
9. 10k 行、16/128 维的重复查询；
10. 进程内和协议级耗时分别统计。

性能实验至少三次重复并报告中位数；首次构建、warm-up 和稳态查询分开统计。探针自身的内存和时间不得计入数据库指标。

## 第五阶段：安全构建和检查

在不改变正式环境的前提下，如果工具已经存在，可以建立临时 sanitizer 构建：

```bash
g++ -O1 -g -std=c++17 -Wall -Wextra \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -o /tmp/seekdb-cpp-agent-7de/minivectordb-asan \
  minivectordb.cpp
```

只有真实运行过的 sanitizer、协议测试或 benchmark 才能记录为通过。缺少工具时写“未测”，不能自行安装依赖后把结果归入原环境。

## 第六阶段：证据材料规则

先阅读并沿用现有 schema：

- `agent/conversation.json`
- `agent/LEARNING_MAP.json`
- `agent/REVIEW_GUIDE.md`
- `agent/EVIDENCE_AUDIT.json`
- `agent/cpp-validation-20261112.json`

严格区分：

- 用户原话；
- Codex 编写的 Prompt；
- DeepSeek 实际执行记录；
- 代码阅读结论；
- 历史实验结果；
- 本次重新实测结果。

没有原始日志的数字只能写成历史记录或未验证，不能写成当前性能。材料必须能从目标 commit 和日志路径追溯，不能使用悬空引用。

## 第七阶段：交付和提交

只允许提交 Agent Prompt 或学习材料；运行时代码必须保持目标 commit 的内容不变。提交前执行：

```bash
git diff --check
git status --short
git diff --stat
```

提交信息建议：

```text
docs(agent): add C++ prompt for 7de2665
```

如果用户已经授权上传，则推送到：

```bash
git push origin HEAD:2026-competition-round2-cpp
```

最终报告必须给出：

1. 目标代码 commit；
2. 实际修改的材料文件；
3. 运行时代码是否保持不变；
4. 实际执行的构建和验证命令；
5. 原始结果和日志路径；
6. 未验证项和风险；
7. 最终提交 commit id；
8. 推送后的远程分支和 HEAD。

不得把 Agent Prompt 文档提交描述成 C++ 性能优化提交，也不得把历史 Python 结果冒充当前 C++ 实测。
