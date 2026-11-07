# AI Column DML 验证记录

本记录区分已运行结果与待验证代码，不代表赛题完成或评分结论。

## 2026-11-07 已取得的 Linux 证据

- `526f2df27f753fcbf4cf413bbac51db769fe25c2` 加上临时诊断日志仍在全新实例复现 -4016。
- 原始错误位于 `ObDMLService::init_das_dml_rtdef()`：WIP 把 `ref_table_id` 从 `index_tid_` 改为 `table_id_`，导致 `(table_loc_id, ref_table_id)` 查找失败；缺失 `loc_meta` 时返回 `OB_ERR_UNEXPECTED`。错误发生在 INSERT 运行定义初始化，早于行级 AI Hook。
- 恢复引用 ID 后，独立实例 Bootstrap 和 `SELECT 1` 成功，`ai_column_ddl.test` 通过。对应提交 `62adf37f3371b91fa146a10a0093e253e370b898` 已推送。
- AI INSERT 的 4006 来自 `ObFixedArrayImpl::push_back()`，日志为 `array not init, capacity=0`。为三个任务列数组分配容量后，单行 INSERT 真实产生 PENDING 任务。
- 实际运行过批量 INSERT + ROLLBACK、依赖更新、主键变更、删除清理和普通表 DML。事务内任务数从 1 变为 3，回滚后业务行及任务均恢复为 1；主键变更后任务谓词指向新主键；删除后任务为 0；普通表 DML 不产生任务。
- `ai_column_task_transaction.test` 在 Linux 执行，输出与预期的 `2 / 0 / 0` 一致。对应提交 `268d29a9d4c9114f8705bea5540ab5fe755a2a27` 已在随后推送的 `de9efa5f0fada064b77ac4798718066d7b9ad6aa` 历史中。
- 完整 `ai_column_t1_dml.test` **失败**：后台 worker 未实现，结果始终未完成，不能把任务写入检查当作完整 T1 通过。

以上运行产物含临时诊断日志，版本字符串仍来自旧 CMake 配置。它们不是最终发布构建。稳定服务端口 31881 未被停止；后续调试使用独立的 31885 实例。

## DML 失效和赋值保护

依赖或主键被赋值时，在 resolver 中生成 AI 结果列的隐式 NULL 赋值；先生成该赋值，再分析向量索引相关更新。移除在 DAS 行缓冲写入后直接改 datum 的旧失效逻辑。普通 UPDATE 执行路径负责结果与索引变更以及回滚。

增加用户 AI 列赋值检查，覆盖 UPDATE、INSERT VALUES、INSERT SELECT、ON DUPLICATE KEY UPDATE。INSERT DEFAULT 仍表示未提供结果；UPDATE DEFAULT 不能作为修改结果的入口。内核内部 SQL 保留发布结果能力。

新增 `ai_column_dml_guards.test`：先在普通 JSON 列中保存非 NULL 结果，再改为 AI 列，以检查真实失效、无关更新保留、回滚恢复和直接赋值拒绝。本次已在新构建的 Windows 实例执行全部 SQL 和断言，包含 8 个预期的 3105 拒绝错误，结果通过。

Windows 本地检查：

- 当前分支 CMake 配置成功。
- 系统表生成测试 `unittest/share/inner_table/test_ai_column_catalog.py` 通过。
- DML engine 与 DML resolver 两个 Unity 编译单元使用 Clang 做语法/类型检查。检查复用了已有 Rust 导出头文件，并将既有告警从错误降为警告；不能代替完整构建或 Linux 运行验证。
- 首次完整 Windows 构建被新版 Clang 对旧代码的告警和 Rust 工作目录权限阻塞。随后使用独立 Rust 构建目录、缓存依赖，并仅在本地生成的编译规则中降级既有告警，继续完整编译；这些本地设置不修改比赛源码或 File SQL。

## 本次新增的整题 WIP

- `AiColumnWorker` 在 Bootstrap 和 Schema 就绪后启动，服务停止时先停止并等待它退出。两个执行者通过任务行的条件更新领取工作，60 秒租约到期后可以重新领取；外部请求限制为 30 秒。
- 业务 DML 与任务写入继续共用原事务。每次行变更生成新的请求标识；同一行本次触发的多个 AI 列共用该标识。任务目录新增执行状态、租约标识和暂存结果；暂存结果持久化后即使进程退出也可以继续发布。
- 模型执行复用原有模型、Endpoint 和提供方实现。类型提示通过系统消息传递，原始 prompt 单独求值。AI 列禁用客户端内部隐式重试，由任务目录统一记录重试次数；原有 AI 函数仍保留原来默认行为。
- 发布前校验 AI 列自身的 Schema 版本。先锁业务行，再锁同组任务；全部成员达到 READY/FAILED 终态后，在同一事务更新结果和隐藏状态，并删除任务。迟到响应必须同时匹配请求标识、领取标识和 RUNNING 状态才能暂存。
- SQL NULL 输入沿用原有 AI 函数的 NULL 传播语义；成功 NULL 的隐藏状态为 READY。失败结果为 SQL NULL 且隐藏状态为 FAILED，严格读取依据状态判断。
- `ALTER TABLE ... RETRY AI COLUMN ... WHERE ...` 解析为经过目标校验的事务性 UPDATE。仅重建目标列任务；未引用的 AI 列保持原值。未加反引号的 `status` 表示目标 AI 列状态，反引号引用继续表示业务列。RETRY 与其他 ALTER 动作混用会拒绝。
- `ai_column_strict_read` 默认为 OFF。计划记录语句引用的 AI 列；执行时使用调用方事务和相同快照检查整列，含当前事务自己的未提交修改。普通列不触发检查。
- 失效和发布都经过普通 UPDATE 路径，让向量及其他索引复用内核维护逻辑。主键更新覆盖拆分 DELETE/INSERT 和同 Tablet 的完整行缓冲两条路径；主键名称及字符串值正确转义。
- 新增 `ai_column_recovery.test`，覆盖租约过期恢复、特殊字符主键、严格读取的事务可见性、单列 RETRY 回滚和成功 NULL 状态。本次已实际执行该脚本的 SQL、轮询和结果比对，结果通过；租约过期 fixture 不等于真正的进程崩溃恢复。

### 2026-11-07 本次 Windows 实测

完整原生 `seekdb.exe` 编译和链接成功；全新数据目录 Bootstrap、`SELECT 1` 成功。最终功能验证实例为本地端口 2959 的 `ai-full-run-20261107-8`。程序版本横幅仍来自配置时的 de9efa5，但编译输入是本次 DML 工作区，新增 worker、RETRY 和严格读取已由实际执行验证。

本机没有正式 mysqltest 可执行程序，使用 PyMySQL 适配器执行现有 `.test` 中的 SQL、轮询、预期错误和 Mock 请求断言，并比对对应 `.result` 的查询结果。数值显示 `0.0` 与 mysqltest 的 `0` 做等值格式处理。没有把适配器执行冒充为正式 Linux mysqltest。

| 用例 | 实际执行 SQL 数 | 结果 |
| --- | ---: | --- |
| ai_column_ddl | 30 | 通过 |
| ai_column_ddl_extended | 25 | 通过 |
| ai_column_dml_guards | 33 | 通过 |
| ai_column_task_transaction | 13 | 通过 |
| ai_column_t1_dml | 52 | 通过 |
| ai_column_worker | 43 | 通过 |
| ai_column_retry_strict | 48 | 通过 |
| ai_column_vector_index | 35 | 通过 |
| ai_column_recovery | 38 | 通过 |

共 317 条脚本 SQL，另有综合测试验证：真实任务分组、提交前不请求模型、批量回滚、无关更新、迟到结果、主键更新、删除回滚、失败重试次数、成功 NULL、过期领取、创建普通索引期间的任务、向量失效／回滚／重新生成。普通表 INSERT、UPDATE、DELETE 不增加 AI 任务。

系统表真实生成测试和 `git diff --check` 通过。C++ parser/resolver/schema 的 gtest 二进制未执行。

本次运行暴露并修复了以下问题：

- Windows 内部 SQL 结果缓冲区需要 16 字节对齐，worker 和严格检查的局部结果对象显式对齐。
- `obj_accuracy_check` 对整数和 JSON 不改写结果指针；按原接口约定初始化为转换结果，避免合法输出被误判为 -4016。
- 底层任务覆盖写不支持 LOB，暂存结果改为最大长度 VARCHAR；生成 SQL 字面量前限制模型结果字节数，保证十六进制表示可以容纳，超限按失败重试处理。
- 完成任务可能已被后台删除。清理任务改为复用业务会话的条件 DELETE，避免构造不存在的旧行触发 4377；内层语句标记为 AI 嵌套 SQL，保留外层事务及快照，避免内层结束时产生 -4016。

本地构建沿用 Clang 21、独立 Rust 缓存及仅作用于构建目录的旧告警降级。增量重配置遇到 Windows 子进程写生成 parser 文件被拒绝：已单独运行真实 parser 和系统表生成器，再用现有 Ninja 清单编译；对比前后生成结果后，仅避免了内容相同文件的重复编译。这些处理未修改仓库构建配置或 File SQL 源码。

本轮没有连接 Linux，也没有向比赛平台提测。

**必须使用全新测试实例。** 本次修改了内部任务表 Schema，尚未实现旧实例的系统表迁移；不能直接把旧数据目录当作本次功能的有效验证环境。

## 恢复服务器连接后的验证

先保存服务器尚未提交的临时诊断改动，再按正常流程同步 DML 分支；不能用强制重置覆盖工作。重新配置构建以更新版本信息。以下命令分别在仓库根目录执行，假设独立实例监听 31885，测试客户端路径与此前服务器一致。

```sh
bash build.sh release --make -j4
```

用新的独立数据目录启动该构建（下面命令按时间创建新目录，不覆盖旧实例）：

```sh
build_release/src/observer/seekdb --base-dir=/tmp/seekdb-ai-column-$(date +%Y%m%d-%H%M%S) --port=31885 --parameter memory_budget=4G --parameter log_disk_size=2G --parameter datafile_size=1G --parameter datafile_maxsize=4G
```

```sh
deps/3rd/u01/obclient/bin/obclient -h127.0.0.1 -P31885 -uroot -e 'SELECT 1; CREATE DATABASE IF NOT EXISTS test;'
```

确认 Bootstrap、`SELECT 1` 后执行：

```sh
python3 unittest/share/inner_table/test_ai_column_catalog.py
```

```sh
mkdir -p /tmp/ai-column-validation
```

```sh
deps/3rd/u01/obclient/bin/mysqltest --host=127.0.0.1 --port=31885 --user=root --database=test --tmpdir=/tmp/ai-column-validation --logdir=/tmp/ai-column-validation --test-file=tools/deploy/mysql_test/test_suite/ai_column/t/ai_column_ddl.test --result-file=tools/deploy/mysql_test/test_suite/ai_column/r/mysql/ai_column_ddl.result
```

```sh
deps/3rd/u01/obclient/bin/mysqltest --host=127.0.0.1 --port=31885 --user=root --database=test --tmpdir=/tmp/ai-column-validation --logdir=/tmp/ai-column-validation --test-file=tools/deploy/mysql_test/test_suite/ai_column/t/ai_column_task_transaction.test --result-file=tools/deploy/mysql_test/test_suite/ai_column/r/mysql/ai_column_task_transaction.result
```

```sh
deps/3rd/u01/obclient/bin/mysqltest --host=127.0.0.1 --port=31885 --user=root --database=test --tmpdir=/tmp/ai-column-validation --logdir=/tmp/ai-column-validation --test-file=tools/deploy/mysql_test/test_suite/ai_column/t/ai_column_dml_guards.test --result-file=tools/deploy/mysql_test/test_suite/ai_column/r/mysql/ai_column_dml_guards.result
```

完整 T1 脚本启动 Mock 时依赖 `tools/deploy` 工作目录：

```sh
cd tools/deploy && ../../deps/3rd/u01/obclient/bin/mysqltest --host=127.0.0.1 --port=31885 --user=root --database=test --tmpdir=/tmp/ai-column-validation --logdir=/tmp/ai-column-validation --test-file=mysql_test/test_suite/ai_column/t/ai_column_t1_dml.test --result-file=mysql_test/test_suite/ai_column/r/mysql/ai_column_t1_dml.result
```

worker、重试、严格读取、向量索引和恢复用例在 `tools/deploy` 目录执行：

```sh
for t in ai_column_worker ai_column_retry_strict ai_column_vector_index ai_column_recovery; do ../../deps/3rd/u01/obclient/bin/mysqltest --host=127.0.0.1 --port=31885 --user=root --database=test --tmpdir=/tmp/ai-column-validation --logdir=/tmp/ai-column-validation --test-file=mysql_test/test_suite/ai_column/t/$t.test --result-file=mysql_test/test_suite/ai_column/r/mysql/$t.result || break; done
```

## 仍需验证或完善的边界

- 新增完整功能尚无 Linux 运行结论；之前 Linux 通过的 DDL 和任务事务检查只覆盖旧提交。
- 正式 Linux Mysqltest、真实进程重启恢复、并发更新压力、HTTP 超时／429／异常响应等专项仍待验证。本地已覆盖向量 ANN、迟到结果、Mock 故障和租约过期 fixture；这些不能替代 Linux 压力与崩溃恢复测试。
- 当前按赋值列判断依赖触发；依赖赋值为原值时仍可能重算。内部会话写入（如触发器）的 DML Hook 处理也需进一步验证。
- AI 列定义变更后的旧任务会丢弃；旧实例迁移、已有行的自动重新入队以及较长主键的任务谓词支持尚需完善。
- 领取和发布的 SQL 失败会保留持久任务等待后续扫描；发布遇到持续约束错误时的终态处理仍需专项验证。
- 严格读取的视图投影范围、SQL 模式及字符集边界需要补充覆盖。
- 独立重建指定稳定提交与原始 WIP 的严格同配置对照实验尚未完成；现有稳定实例可查询不等于完成该对照。

该分支可作为用户手动提测的 WIP 候选，不能标记为整题完成或保证分数。代理推送只发布代码到 GitHub，不会代替用户提交比赛平台。
