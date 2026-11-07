# AI Column DML 验证记录

本记录区分已运行结果与待验证代码，不代表赛题完成或评分结论。

## 2026-11-07 已取得的 Linux 证据

- `526f2df27f753fcbf4cf413bbac51db769fe25c2` 加上临时诊断日志仍在全新实例复现 -4016。
- 原始错误位于 `ObDMLService::init_das_dml_rtdef()`：WIP 把 `ref_table_id` 从 `index_tid_` 改为 `table_id_`，导致 `(table_loc_id, ref_table_id)` 查找失败；缺失 `loc_meta` 时返回 `OB_ERR_UNEXPECTED`。错误发生在 INSERT 运行定义初始化，早于行级 AI Hook。
- 恢复引用 ID 后，独立实例 Bootstrap 和 `SELECT 1` 成功，`ai_column_ddl.test` 通过。对应提交 `62adf37f3371b91fa146a10a0093e253e370b898` 已推送。
- AI INSERT 的 4006 来自 `ObFixedArrayImpl::push_back()`，日志为 `array not init, capacity=0`。为三个任务列数组分配容量后，单行 INSERT 真实产生 PENDING 任务。
- 实际运行过批量 INSERT + ROLLBACK、依赖更新、主键变更、删除清理和普通表 DML。事务内任务数从 1 变为 3，回滚后业务行及任务均恢复为 1；主键变更后任务谓词指向新主键；删除后任务为 0；普通表 DML 不产生任务。
- `ai_column_task_transaction.test` 在 Linux 执行，输出与预期的 `2 / 0 / 0` 一致。对应本地提交 `268d29a9d4c9114f8705bea5540ab5fe755a2a27`；此前推送遇到网络错误，不能假设远端已经拥有此提交。
- 完整 `ai_column_t1_dml.test` **失败**：后台 worker 未实现，结果始终未完成，不能把任务写入检查当作完整 T1 通过。

以上运行产物含临时诊断日志，版本字符串仍来自旧 CMake 配置。它们不是最终发布构建。稳定服务端口 31881 未被停止；后续调试使用独立的 31885 实例。

## 随后的本地改动

依赖或主键被赋值时，在 resolver 中生成 AI 结果列的隐式 NULL 赋值；先生成该赋值，再分析向量索引相关更新。移除在 DAS 行缓冲写入后直接改 datum 的旧失效逻辑。普通 UPDATE 执行路径负责结果与索引变更以及回滚。

增加用户 AI 列赋值检查，覆盖 UPDATE、INSERT VALUES、INSERT SELECT、ON DUPLICATE KEY UPDATE。INSERT DEFAULT 仍表示未提供结果；UPDATE DEFAULT 不能作为修改结果的入口。内核内部 SQL 保留发布结果能力。

新增 `ai_column_dml_guards.test`：先在普通 JSON 列中保存非 NULL 结果，再改为 AI 列，以检查真实失效、无关更新保留、回滚恢复和直接赋值拒绝。预期文件是测试规范，**尚未取得数据库运行结果**。

Windows 本地检查：

- 当前分支 CMake 配置成功。
- 系统表生成测试 `unittest/share/inner_table/test_ai_column_catalog.py` 通过。
- DML engine 与 DML resolver 两个 Unity 编译单元使用 Clang 做语法/类型检查。检查复用了已有 Rust 导出头文件，并将既有告警从错误降为警告；不能代替完整构建或 Linux 运行验证。
- 完整 Windows 构建未通过：旧 zstd 源码触发新版 Clang `-Wnrvo`，Rust 生成 `Cargo.lock` 时遇到权限错误。未为绕过这些问题修改比赛代码或 File SQL。

## 恢复服务器连接后的验证

先保存服务器尚未提交的临时诊断改动，再按正常流程同步 DML 分支；不能用强制重置覆盖工作。重新配置构建以更新版本信息。以下命令分别在仓库根目录执行，假设独立实例监听 31885，测试客户端路径与此前服务器一致。

```sh
bash build.sh release --make -j4
```

用新的独立数据目录启动该构建，确认 Bootstrap、`SELECT 1` 后执行：

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

## 尚未完成，不能据此提测整题

- 后台任务领取、恢复、模型执行、类型转换、自动重试和多列分组发布。
- 每次数据变更的请求标识与定义版本校验；现有任务 request_id 仍为空。
- READY / FAILED 持久状态及条件 RETRY、严格读取。
- 向量索引 ANN 全链路验证，以及真正已有模型结果时的失效与回滚验证。
- 无变化赋值、触发器、跨语句重试、并发更新与迟到结果等边界验证。
- 独立重建指定稳定提交与原始 WIP 的严格同配置对照实验尚未完成；现有稳定实例可查询不等于完成该对照。

后续应先完成并验证 DML 的状态与请求版本基础，再接内核 worker；不得用外部脚本代替数据库内核实现。未经完整回归，不提交官方评测平台。
