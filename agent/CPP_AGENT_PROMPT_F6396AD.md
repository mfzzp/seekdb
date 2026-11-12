# C++ Agent Prompt：`2026-competition-round2-cpp`（代码 `f6396ad`）

面向 seekdb C++ 版本的审计 / 实验 / 学习证据任务。目标代码：

| 项 | 值 |
| --- | --- |
| 仓库 | `https://github.com/mfzzp/seekdb` |
| 分支 | `2026-competition-round2-cpp` |
| 代码 commit | `f6396add14a1e54c48e4de5301da4179b2a8b47d`（`replace snapshot rewrites with journal commits`） |
| 代码文件 | `minivectordb.cpp`（单文件，C++17） |
| 本 Prompt | `agent/CPP_AGENT_PROMPT_F6396AD.md`（分支 HEAD 上的文档提交） |

> 取代 `agent/CPP_AGENT_PROMPT_7DE2665.md`。旧文件针对 `7de2665`，当时每次提交都重写整份 `state.bin`；`f6396ad` 已改成 journal 追加，旧文件的保存/恢复描述不再成立。旧文件保留用于追溯。

## 0. 十分钟快速核对

在写任何结论之前，先把下面六条跑完并保存原始输出：

```bash
# 1) 代码与文档都在同一分支上，且运行时代码未改
git log --oneline -3
git rev-parse HEAD:minivectordb.cpp        # 期望 e2ee8384e3635b9c394460015709817d077feacd
git diff f6396add14a1e54c48e4de5301da4179b2a8b47d -- minivectordb.cpp Dockerfile

# 2) 编译（g++ 13.3.0，已知仅两条 -Wmisleading-indentation）
g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra -o /tmp/mvdb-f6396ad minivectordb.cpp

# 3) 公开用例
python3 protocol_check/run.py \
  --command '["/tmp/mvdb-f6396ad","--data-dir","{data_dir}"]' \
  --output /tmp/public-f6396ad

# 4) torn tail 实验（本次已复现两种后果，先重跑确认，见 §4.1）
# 5) SIGKILL 实验（§4.2）
# 6) 启动重放耗时（§4.3）
```

做不到的项写"未测"，不要用旧记录补齐。

## 1. 角色、范围与硬约束

你是代码审计、实验设计和学习证据助手。默认只写 Prompt 与证据材料；除非用户另行明确授权，**不得修改** `minivectordb.cpp`、`Dockerfile`、`tests/`、`protocol_check/`、数据目录或 Git 历史。

硬约束：

- 服务器没有 GitHub 凭据（无 SSH key、无 `gh`、无 credential helper、无 token 环境变量）。不要在服务器上尝试推送、不要收集或落地任何 token；推送由有凭据的人完成。
- 密码只交互输入，不写进脚本、环境变量、日志或提交内容。
- 不安装新依赖、不改编译器、不动他人工作树。
- 不 force push、不改写历史。

## 2. 环境与工作树

```bash
ssh zzp@10.34.82.174
cd /mnt/HDD2/zzp/oceanbase-competition/seekdb      # 共享主工作树，可能停在别的分支
```

用独立工作树，且落到分支 HEAD（不是 `f6396ad`，因为分支上还有 Prompt 提交）：

```bash
git fetch --no-tags origin \
  2026-competition-round2-cpp:refs/remotes/origin/2026-competition-round2-cpp
git worktree add --detach /mnt/HDD2/zzp/seekdb-audit-f6396ad \
  refs/remotes/origin/2026-competition-round2-cpp
cd /mnt/HDD2/zzp/seekdb-audit-f6396ad
git rev-parse HEAD:minivectordb.cpp    # 必须等于 e2ee8384...
```

工作树已存在时先 `git status --short --branch`，不得覆盖未提交修改。审计基线始终是"分支 HEAD，其 `minivectordb.cpp` blob 等于 `f6396ad` 版本"。

## 3. 已核对的事实（代码阅读 + 一次构建，不等于实测）

| # | 事实 | 证据 |
| --- | --- | --- |
| 1 | `f6396ad` 的父提交是 `7de2665`；它改 `README.md`、`agent/cpp-validation-20261112.json`、`minivectordb.cpp`（+67/−7） | `git show --numstat f6396ad` |
| 2 | 旧路径：`commit()` / `autocommit()` 调用 `save()`，整份 `state.bin` 写临时文件后 `rename` | `git show f6396ad -- minivectordb.cpp` 删除行 |
| 3 | 新路径：每次提交把 SQL 追加到 `/data/journal.log`，`open(O_APPEND)` + 3 次 `write` + `fsync` | `append_journal()` |
| 4 | 记录格式：`u32 magic(0x4d564a31)`、`u32 payload 字节数`；payload = `u32 语句数` + 每条 `u32 长度 + SQL` | `append_journal()` / `replay_journal()` |
| 5 | 上限：单条语句 2^28 字节，单条记录 2^30 字节，语句数 10^6；超限在写入时抛 `storage_error`，重放时 `break` | 同上 |
| 6 | 重放遇到 torn tail / magic 不符 / 长度越界时**静默 break**，不报错、不截断文件 | `replay_journal()` |
| 6a | 本次实测：只截断最后 1 字节、不追加新记录时，重启成功但最后一条 INSERT 被静默丢弃（表为空，退出码 0） | 见 §4.1 的复现记录 |
| 6b | 本次实测：截断后先提交一条新记录，**下一次启动直接失败**（`startup failed: trailing journal SQL`，退出码 1）：坏帧声明的长度延伸进下一条记录，拼出的 SQL 多一个字节，`replay_sql()` 抛错 | 见 §4.1 |
| 7 | `load()`：无 `state.bin` → 直接重放 journal；有 `state.bin` → 先加载快照，再**从头重放整个 journal** | `load()` |
| 8 | `save()` 仍保留，但全文件没有任何调用点（死代码，只剩 `load()` 的兼容读取） | `grep -n "save()" minivectordb.cpp` |
| 9 | 事务：`BEGIN` 复制整个 `State`；语句进 `tx_log`；`COMMIT` 一次 `append_journal(tx_log)`；`ROLLBACK` 恢复副本 | `begin()` / `commit()` / `rollback()` |
| 10 | `main()` 对非 SELECT 语句一律调用 `autocommit(true, sql)`，`changed` 恒为真 | `main()` |
| 11 | `UPDATE` / `DELETE` 后调用 `rebuild()` 全量重建 `keys` | `update()` / `erase()` |
| 12 | `SELECT` 的距离缓存是 `投影数 × 表行数` 的 `double` 矩阵 + 同尺寸的 ready 字节矩阵，按全表行数分配而非候选数 | `select()` |
| 13 | TopK：`limit < 候选数` 时 `nth_element` + `resize` + `sort`；比较器依次是 ORDER BY 各项、主键升序 | `select()` |
| 14 | `ORDER BY` 解析遍历投影列表，命中即覆盖（最后一个匹配获胜）；`ORDER BY distance` 只匹配距离投影 | `select()` |
| 15 | `g++ 13.3.0` 编译成功，仅 `replay_journal()` 与 `main()` 两处 `-Wmisleading-indentation` | 本次构建，需你重跑确认 |

仍然**未验证**、不得写成结论的：`f6396ad` 上重新跑公开用例的结果、SIGKILL 行为、torn tail 行为、journal 增长对启动的影响、真实崩溃后的数据可见性。

## 4. 优先审计项（4.1 已复现，其余为待验证假设）

每条都要"可证伪 + 可复现"。先做 P0，再 P1，最后 P2；失败的尝试同样写进报告。

### 4.1 P0：torn journal 会让数据库丢掉最后一条提交，甚至拒绝启动

重放遇到坏帧直接 `break`，且**不截断**；而 `append_journal` 用 `O_APPEND` 继续往文件尾追加。本次已在 `f6396ad` 上复现两种后果：

- **只截断、不追加**：启动成功，最后一条记录被静默跳过，已返回 `ok` 的 INSERT 不可见（退出码 0）；
- **截断后再提交一条**：下一次启动直接失败，`startup failed: trailing journal SQL`，退出码 1。原因是坏帧头里声明的长度现在覆盖到后一条记录的开头，重放拼出的 SQL 多了一个字节，`replay_sql()` 以"trailing journal SQL"抛错，`main()` 捕获后拒绝启动。

复现（2026-11-12 实测，`g++ 13.3.0`，`CREATE TABLE` + 一条 INSERT 后 `journal.log` 为 116 字节）：

```bash
BIN=/tmp/mvdb-f6396ad
DATA=/tmp/audit-torn
rm -rf "$DATA"
cat <<'JSONL' | "$BIN" --data-dir "$DATA"
{"id":1,"op":"execute","sql":"CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(2));"}
{"id":2,"op":"execute","sql":"INSERT INTO t VALUES (1,'[0,0]');"}
{"id":3,"op":"shutdown"}
JSONL
wc -c "$DATA/journal.log"          # 116
truncate -s -1 "$DATA/journal.log" # 模拟半条记录
# 情形 A：直接重启并查询，最后一条 INSERT 消失，但进程正常
cat <<'JSONL' | "$BIN" --data-dir "$DATA"
{"id":4,"op":"execute","sql":"SELECT id FROM t ORDER BY id;"}
{"id":5,"op":"shutdown"}
JSONL
# 情形 B：先提交一条新记录，再重启
cat <<'JSONL' | "$BIN" --data-dir "$DATA"
{"id":6,"op":"execute","sql":"INSERT INTO t VALUES (2,'[1,1]');"}
{"id":7,"op":"shutdown"}
JSONL
cat <<'JSONL' | "$BIN" --data-dir "$DATA"
{"id":8,"op":"execute","sql":"SELECT id FROM t ORDER BY id;"}
{"id":9,"op":"shutdown"}
JSONL
```

审计要求：重跑并记录两种情形的完整 stdout/stderr、退出码、`journal.log` 字节数与坏帧偏移；判断"静默丢最后一条"是否已经违反题面的持久化承诺；给出修复方向（启动时截断到最后一个完整记录、或追加前检测尾部完整性、或引入 checkpoint），但**不要在本任务中改运行时代码**（除非用户书面授权并单独记录 commit）。

同时检查：`SIGKILL` 落在 `write()` 的哪个阶段会产生半条记录；`fsync` 成功但进程在返回响应前被杀时，客户端看到的失败与磁盘上的记录是否一致。

### 4.2 P0/P1：追加失败后的状态恢复

`commit()` / `autocommit()` 的 catch 是 `st = State{}; tx.reset(); load();`。若 `load()` 在坏数据上再抛异常，异常会冒泡到 `main()` 的 `std::exception` 分支：返回 `internal_error`，但内存里的 `st` 可能已经是空的。审计：

- 在一个损坏的数据目录上制造追加失败（如 `chmod` 只读、磁盘满、或让 `load()` 抛错），观察之后同一进程内的 `SELECT` 是否还能看到数据；
- 确认失败语句是否部分可见、事务是否泄漏；
- 对照 `SIGKILL` 后重启的结果。

### 4.3 P1：`state.bin` 与 journal 的叠加语义

`save()` 不再被调用，因此新版本永远不会更新 `state.bin`；但 `load()` 仍会在快照之上重放**整个** journal。代码隐含的不变量是"`state.bin` 必须是 journal 历史的前缀"，但没有任何检查或版本号。

审计：

- 用 `7de2665` 的二进制造一个旧数据目录（只有 `state.bin`），再用 `f6396ad` 启动，查询是否正确；
- 在旧快照之上用新版本写入若干条，再重启，确认没有重复应用；
- 构造"快照晚于 journal"的场景（例如把较新的 `state.bin` 拷回旧 journal），确认是否重复应用/覆盖；
- 明确回答：这个兼容路径是有意支持升级，还是应该删掉 `save()`/`state.bin`？

### 4.4 P1：journal 无压缩、无 checkpoint

journal 只追加，`UPDATE`/`DELETE` 也各留一条历史 SQL。审计：

- 10k / 100k 次写入后 `journal.log` 的大小与启动重放时间；
- 大量删除后再插入，重放成本是否按历史条数增长；
- 是否有可接受的 checkpoint/压缩方案，代价是什么（崩溃一致性、实现复杂度）。

### 4.5 P2：写入放大与 no-op 提交

`main()` 对非 SELECT 一律 `autocommit(true, sql)`，即使 `UPDATE`/`DELETE` 影响 0 行也会追加一条记录。审计：0 行 UPDATE/DELETE 是否产生 journal 记录、重复执行 1 万次后的文件大小与启动时间。这是事实核对，不是必须修的问题。

### 4.6 P2：事务复制、rebuild 与距离缓存

- `BEGIN` 复制整个 `State` 的内存峰值：10k/100k 行、128 维时是多少？
- `UPDATE`/`DELETE` 后 `rebuild()` 的复杂度与实测耗时；
- `SELECT` 的 `投影数 × 表行数` 距离缓存：过滤到 0.1% 时是否仍按全表分配？峰值 RSS 与耗时；
- `nth_element` 之前是否已经算了全量距离，候选集能否先缩小。

## 5. 正确性与协议检查

- 公开 20 例 + 12 个额外用例（`protocol_check/run.py`，可加 `--package tests/extra_cases`，以仓库实际参数为准）；
- `exact` 与 `ann` 两种 `search_mode` 结果一致；
- 多距离投影、别名、`ORDER BY distance`、同名歧义；
- 并列距离按主键升序；`LIMIT 0`、超行数 LIMIT、无 `ORDER BY` 的行序；
- `UPDATE` 的 `affected_rows` 语义（匹配 vs 实际改变）、多行 INSERT 原子性、重复主键；
- 事务内 DDL、失败语句后的连接可用性、无事务 `COMMIT`/`ROLLBACK`；
- 协议：`ready` 握手、空行、非法 JSON、`id` 原样回传、`shutdown` 退出码、stdout 纯净；
- JSON 数字精度（`setprecision(15)`）、`-0`、非有限值、BIGINT 不经过浮点。

每条结论都要有命令、原始输出、以及"这是实测还是代码阅读"的标记。

## 6. 实验记录规则

每个实验一条记录，包含：唯一 id、日期、执行者、baseline/candidate commit（本任务 baseline 固定 `f6396ad`，若改代码需用户授权并另记）、可证伪假设、数据规模/维度/过滤/TopK/种子、完整命令、原始 stdout/stderr 路径、正确性结果、before/after 中位数与 P95、峰值 RSS、启动与恢复耗时、失败尝试、是否保留。

性能实验至少三次重复取中位数；首次构建、warm-up、稳态分开；探针自身开销不计入数据库指标。测量 RSS 用 `/proc/<pid>/status` 的 `VmHWM`，协议级计时用独立进程 + stdin/stdout。

## 7. 证据材料

沿用现有 schema：

- `agent/conversation.json`
- `agent/LEARNING_MAP.json`
- `agent/REVIEW_GUIDE.md`
- `agent/EVIDENCE_AUDIT.json`
- `agent/cpp-validation-20261112.json`

严格区分：用户原话 / 本 Prompt / 实际执行记录 / 代码阅读结论 / 历史结果 / 本次实测。没有原始日志的数字只能写成历史或未验证；所有引用要能追到 commit 与日志路径。

## 8. 交付与提交

只提交 Prompt 或学习材料；运行时代码必须保持 `f6396ad` 的内容：

```bash
git diff f6396ad -- minivectordb.cpp Dockerfile    # 必须为空
git diff --check
git status --short
git diff --stat
```

提交信息示例：

```text
docs(agent): sharpen the f6396ad audit prompt
```

推送（由有凭据的人执行，服务器上没有凭据）：

```bash
git push origin HEAD:2026-competition-round2-cpp
```

最终报告必须给出：

1. 目标代码 commit 与分支 HEAD；
2. 修改的材料文件；
3. `minivectordb.cpp` 未变的证明（`git diff` 为空 + blob hash）；
4. 实际执行的构建/测试命令与原始输出；
5. 每个 P0/P1 假设的结论：确认 / 未复现 / 未测；
6. 未验证项与风险；
7. 最终提交 commit id 与远程分支 HEAD。

## 9. 禁止事项

- 把文档提交描述成性能优化提交；
- 把历史 Python 或旧 C++ 结果冒充本次实测；
- 未真实执行的构建/测试/sanitizer/benchmark 写成"通过"；
- force push、改写历史、覆盖他人工作树；
- 提交 `minivectordb.cpp`、`Dockerfile`、`state.bin`、`journal.log`、构建产物或测试数据；
- 收集、保存或提交任何 GitHub token / 密码。
