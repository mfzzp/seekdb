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

# 4) torn tail 回归（已修复，按 §4.1 重跑修复后用例）
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
| 5 | 上限：单条语句 2^28 字节，单条记录 2^30 字节，语句数 10^6；写入超限抛 `storage_error`，重放时完整帧内 count 不可读或超限抛 `storage_error` | 同上 |
| 6 | 重放：torn tail（帧头不足 8 字节、payload 超出文件、statement 越界）截断到最后一个完整帧；magic 错误或完整帧内损坏抛 `storage_error` | `replay_journal()`（本次已修复） |
| 6a | 修复前实测：只截断最后 1 字节、不追加新记录时，重启成功但最后一条 INSERT 被静默丢弃（表为空，退出码 0） | §4.1 的修复前记录 |
| 6b | 修复前实测：截断后先提交一条新记录，**下一次启动直接失败**（`startup failed: trailing journal SQL`，退出码 1） | §4.1 |
| 6c | 修复后实测：截断 1 字节 → 重启 → 追加 → 再重启，启动成功；截断的 INSERT 丢失，后续写入保留 | §4.1 的修复后记录 |
| 7 | `load()`：无 `state.bin` → 直接重放 journal；有 `state.bin` → 先加载快照，再**从头重放整个 journal** | `load()` |
| 8 | `save()` 仍保留，但全文件没有任何调用点（死代码，只剩 `load()` 的兼容读取） | `grep -n "save()" minivectordb.cpp` |
| 9 | 事务：`BEGIN` 复制整个 `State`；语句进 `tx_log`；`COMMIT` 一次 `append_journal(tx_log)`；`ROLLBACK` 恢复副本 | `begin()` / `commit()` / `rollback()` |
| 10 | `main()` 对非 SELECT 语句一律调用 `autocommit(true, sql)`，`changed` 恒为真 | `main()` |
| 11 | `UPDATE` / `DELETE` 后调用 `rebuild()` 全量重建 `keys` | `update()` / `erase()` |
| 12 | `SELECT` 的距离缓存是 `投影数 × 表行数` 的 `double` 矩阵 + 同尺寸的 ready 字节矩阵，按全表行数分配而非候选数 | `select()` |
| 13 | TopK：`limit < 候选数` 时 `nth_element` + `resize` + `sort`；比较器依次是 ORDER BY 各项、主键升序 | `select()` |
| 14 | `ORDER BY` 解析遍历投影列表，命中即覆盖（最后一个匹配获胜）；`ORDER BY distance` 只匹配距离投影 | `select()` |
| 15 | `g++ 13.3.0` 编译成功，**0 条警告**（`-Wmisleading-indentation` 与未使用的 `table_of(const State&)` 已清理） | 本次构建 |

仍然**未验证**、不得写成结论的：journal 中间帧损坏后的行为（按设计会拒绝启动）、SIGKILL 落在 write/fsync 精确阶段的行为、journal 增长对启动的影响、`state.bin` 与 journal 的叠加语义。公开用例与额外用例已在修复后实测通过，见 §4.1。

## 4. 优先审计项（4.1 已修复，其余为待验证假设）

每条都要"可证伪 + 可复现"。先做 P0，再 P1，最后 P2；失败的尝试同样写进报告。

### 4.1 P0：torn journal（已修复：`fix(storage): recover torn journal tails safely`）

**修复前**（`f6396ad`）实测两种后果：

- **只截断、不追加**：启动成功，最后一条记录被静默跳过，已返回 `ok` 的 INSERT 不可见（退出码 0）；
- **截断后再提交一条**：下一次启动直接失败，`startup failed: trailing journal SQL`，退出码 1。原因是坏帧头里声明的长度现在覆盖到后一条记录的开头，重放拼出的 SQL 多了一个字节，`replay_sql()` 抛错，`main()` 捕获后拒绝启动。

**修复内容**（`replay_journal()`）：

- 记录 `valid_end` = 最后一个完整帧的结束偏移；
- 帧头不足 8 字节、payload 长度超过文件剩余、statement 越界、帧内字段不完整 → 视为 torn tail；启动时 `ftruncate(valid_end)` + `fsync`，失败则抛 `storage_error`；
- `magic` 错误、`bytes > 2^30`、完整帧内 `count` 不可读或 >10^6 → 抛 `storage_error`，不静默吞掉；
- `replay_sql` 解析失败仍抛 `storage_error`（`trailing journal SQL`），不会被当成正常结束；
- 截断后继续写入正常。

**修复后实测**（2026-11-12，`g++ 13.3.0`）：

| 场景 | 结果 |
| --- | --- |
| 截断 1 字节 → 重启 → 追加 → 再重启 | 启动成功；截断的 INSERT 丢失，后续 INSERT 保留 |
| 追加 4 字节半帧头 → 重启 | 启动时截断回 115 字节，启动成功 |
| 截断 1..7 字节各跑一遍 | 两次重启均成功，无 `startup failed` |
| SIGKILL 于 2 万行 INSERT 期间 | 重启成功，之后可继续写入 |
| 公开 20 例 / 额外 12 例 | 20/20、12/12 PASS |

**尾部损坏 vs 中间损坏**（必须分开判断，不能把中间损坏当成尾部截断）：

| 位置 | 判据 | 行为 |
| --- | --- | --- |
| 文件末尾 | 帧头只剩 1..7 字节、payload 长度超过文件剩余、statement 越界、count 字段读不全 | 视为 torn tail：截断到 `valid_end`，继续启动 |
| 文件中间（后续还有记录） | 完整帧的 `magic` 错误、`bytes > 2^30`、完整帧内 count 不可读或 >10^6、`replay_sql` 解析失败 | 抛 `storage_error`，拒绝启动；**不得**静默截断丢数据 |

**仓库回归测试**：`tests/test_journal_recovery.py`（用 g++ 构建真实二进制，走 stdin/stdout），覆盖：

- 末尾截断 1..7 字节 → 重启 → 追加 → 再重启；
- 只剩 1..7 字节帧头；
- 只剩部分 payload；count 字段不完整；
- 尾部损坏后存在下一条合法记录（旧崩溃场景）；
- 中间帧 magic / 荒谬 count / SQL 损坏 → 明确 `storage_error`、无 ready 行。

运行：

```bash
cd /mnt/HDD2/zzp/seekdb-cpp-integrated
python3 -m unittest tests.test_journal_recovery -v
```

**持久性边界（必须区分，不能混为一谈）**：

- 正常提交顺序是 `append_journal()`（write + `fsync`）→ 返回 → `main()` 写响应，因此**收到 `ok` 的写入一定已有完整帧**；
- 启动截断只会落在"没有返回 ok"的半条记录上；"clean shutdown 后人为 `truncate` 掉一条已确认记录"是**数据损坏**，不是崩溃一致性场景，丢数据是预期的，不能用它证明线上会丢已确认写入；
- 仍未实现：帧校验和、commit marker、checkpoint/压缩；journal 中间帧损坏仍会拒绝启动（这是"不静默吞掉"的刻意选择）。

**仍需验证**：SIGKILL 落在 `write`/`fsync` 精确阶段的字节级行为（fault injection 或 strace）、崩溃后 journal 的实际内容、长时间写入后的重放耗时。

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

journal 只追加，`UPDATE`/`DELETE` 也各留一条历史 SQL；没有大小上限，也没有快照。长期写入后文件与重放时间都会线性增长。要求（作为后续实现项，不在本轮完成）：

- 定义 journal 大小上限（例如 64 MiB 或按行数/字节数触发）；
- 达到上限时生成快照：把当前 `State` 序列化为 `state.bin.tmp`，`fsync`，`rename` 原子替换 `state.bin`；
- 快照 `fsync` 成功后再截断 journal，只保留快照之后的记录（顺序不能反，否则崩溃会丢已确认写入）；
- 恢复时先加载 `state.bin`，再重放它之后的 journal；
- 必须测试崩溃发生在 checkpoint 前、序列化中、rename 前、rename 后、journal 截断前的行为；
- 记录 10k / 100k 次写入后 `journal.log` 大小、启动重放时间、checkpoint 前后峰值 RSS。

### 4.5 P1：journal 校验和（建议）

当前格式是 `magic(4) + payload_length(4) + payload`，没有 CRC，无法区分"真实截断"和"磁盘内容被改写"。建议后续升级为：

```text
magic(4) + version(2) + flags(2) + payload_length(4) + crc32(4) + payload
```

- 写入端计算 CRC32，`fsync` 前完成；
- 恢复端先校验 CRC：不匹配时按"损坏"抛 `storage_error`，而不是截断；
- 需要版本迁移：旧 journal 没有 CRC，必须定义"只读兼容"或一次性转换；
- 测试：翻转 payload 任意字节、翻转 CRC 自身、截断到任意偏移，分别验证"损坏"与"截断"被正确区分。

### 4.6 P2：写入放大与 no-op 提交

`main()` 对非 SELECT 一律 `autocommit(true, sql)`，即使 `UPDATE`/`DELETE` 影响 0 行也会追加一条记录。审计：0 行 UPDATE/DELETE 是否产生 journal 记录、重复执行 1 万次后的文件大小与启动时间。这是事实核对，不是必须修的问题。

### 4.7 P2：事务复制、rebuild 与距离缓存

- `BEGIN` 复制整个 `State` 的内存峰值：10k/100k 行、128 维时是多少？
- `UPDATE`/`DELETE` 后 `rebuild()` 的复杂度与实测耗时；
- `SELECT` 的 `投影数 × 表行数` 距离缓存：过滤到 0.1% 时是否仍按全表分配？峰值 RSS 与耗时；
- `nth_element` 之前是否已经算了全量距离，候选集能否先缩小。

## 5. 正确性与协议检查

- 公开 20 例 + 12 个额外用例（`protocol_check/run.py`，可加 `--package tests/extra_cases`，以仓库实际参数为准）；
- journal 恢复：`python3 -m unittest tests.test_journal_recovery -v`（8 个用例：尾部 1..7 字节、部分帧头、部分 payload、count 不完整、追加后连续重启、中间 magic/count/SQL 损坏）；
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

运行时代码默认不改；确需修改时必须获得单独授权，并满足：

- 只改 `minivectordb.cpp`（或用户明确指定的文件），单独一个 commit，不与 Prompt/证据混在一起；
- 修改前后都记录 `sha1sum minivectordb.cpp`、`git diff --stat`、构建命令与 `-Wall -Wextra` 警告数；
- 运行 `tests/test_journal_recovery.py`、公开 20 例、额外 12 例，记录原始 stdout/stderr；
- 失败或未执行的测试写"未测"，不得写成通过。

提交前检查：

```bash
git diff --check
git status --short
git diff --stat
g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra -o /tmp/mvdb-check minivectordb.cpp
python3 -m unittest tests.test_journal_recovery -v
```

**完成条件（全部满足才算完成）**：

- 公开用例 20/20；
- 额外用例 12/12；
- 尾部截断 1..7 字节均可恢复；
- 截断后追加写入并连续重启成功；
- 中间帧损坏明确返回 `storage_error`，不得静默丢数据；
- 没有 `database exited before response`；
- `git diff --check` 通过；
- 运行时代码、Prompt、测试文件的变更范围清晰（`git show --stat`）；
- 每个结论都有原始日志，或明确标记为未验证。

推送（由有凭据的人执行，服务器上没有凭据）：

```bash
git push origin HEAD:2026-competition-round2-cpp
```

**实验辅助文件**：`journal-tail-fix.patch` 是 Codex 生成的实验补丁，已移到 `/mnt/HDD2/zzp/seekdb-cpp-integrated-experiments/`，不属于正式交付内容；正式代码以 `git commit` 中的 `minivectordb.cpp` 为准。

最终报告必须给出：

1. 当前 HEAD 与目标 commit；
2. 修改文件与 `git show --stat`；
3. 运行时是否修改、修改内容与授权来源；
4. 实际执行的构建/测试命令与原始输出；
5. 每个 P0/P1 项的结论：确认 / 未复现 / 未测；
6. 未验证项与风险；
7. 最终提交 commit id 与远程分支 HEAD。

## 9. 禁止事项

- 把文档提交描述成性能优化提交；
- 把历史 Python 或旧 C++ 结果冒充本次实测；
- 未真实执行的构建/测试/sanitizer/benchmark 写成"通过"；
- force push、改写历史、覆盖他人工作树；
- 提交 `minivectordb.cpp`、`Dockerfile`、`state.bin`、`journal.log`、构建产物或测试数据；
- 收集、保存或提交任何 GitHub token / 密码。
