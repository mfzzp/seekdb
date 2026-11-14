# 写入路径优化：2026-11-14

本轮基于已推送的 `edde4ac`，优化写入路径并保持两次成功同步。当前总体实现见 [IMPLEMENTATION.md](IMPLEMENTATION.md)。

## 先纠正截图口径

截图的 0.958 s / 1.304 s 来自早期不同入口的测试：旧版直接调用 Python Database，新版使用 C++ JSONL，后者计入了 Python 向量格式化、SQL 拼接和 JSON 编码。仓库先前的 [WRITE_LIMITS_20261114.md](WRITE_LIMITS_20261114.md) 与 RESULTS.md 已撤回“同口径慢 36%”的结论。此次不以这两个数字作为严格基线，也不把本机秒数与它们相减。

## 改了什么

1. 复用读写方式打开的 WAL 描述符，把一笔事务的原有帧连续编码后完整写入，避免每批重复打开、刷新流、再打开和关闭 WAL。
2. CRC32C 改为 SSE4.2 加速；无该指令时使用查表实现。磁盘格式、CRC 多项式、事务帧与对齐不变。
3. MemTable 维护累计字节数，避免每次提交遍历全部记录；WAL 维护累计文件长度，避免每次提交 stat 正在写入的文件。首次打开时读取恢复后的长度，checkpoint 成功截断后清零。
4. 显式事务中的 INSERT 也采用单条语句撤销新增行，不再额外复制整张表。BEGIN 的事务快照仍保留，UPDATE/DELETE 的语句快照仍保留；不是完整事务写集重构。
5. 修复失败语句的向量边界：同一事务中先前成功但仍在缓冲区的向量不能被当作实际文件长度，否则截断操作可能扩文件产生空洞。逻辑长度、物理长度分别恢复。
6. 向量先完整写入并同步，再写入其对应的完整 WAL 事务并同步。短写和 EINTR 重试；提交 I/O 错误或提交后的维护错误会冻结引擎，拒绝继续执行、回滚截断或 checkpoint，要求重启恢复。避免 WAL 可能已提交时截断其向量。

每次成功的非空提交仍有向量和 WAL 两次 fdatasync，没有关掉 WAL、CRC、同步或改变每批行数。故障注入确认两次调用成功且 WAL 写入发生在向量同步之后。checkpoint 和关闭也单独计时，没有把它们省略。

## 对照方法与限制

Windows / MSYS2 GCC 16.2.0，Intel i5-1135G7；两版相同 `-D_GNU_SOURCE -O3 -DNDEBUG -std=c++17`。每组两版各三次，串行且交替顺序；数据生成、编译、其他回归均不在最终写入计时窗口内。每轮使用新目录、相同预生成 JSONL、相同向量、相同批量与提交次数。独立生成器为 numpy 固定种子高斯数据，4 列；空表先 CREATE VECTOR INDEX，因此不包含已训练 IVF/PQ 逐条维护。

本机探针确认旧代码对只读 WAL 描述符执行 fdatasync 返回 EACCES，而返回值原来被忽略。为公平对照，本地基线只把 `open(wal.c_str(),O_RDONLY)` 改为 O_RDWR；不改算法和数据。候选检查同步结果并直接复用读写描述符。不能用未修正基线在 Windows 上失败的同步来代表相同持久化工作。Linux 复测应从原始 edde4ac 编译，单独验证当地行为。

**本轮较大的墙钟收益受 Windows/MSYS 文件打开、关闭和状态查询成本影响，不能直接承诺 Linux 获得相同比例。** 同步等待没有消失，CPU、文件操作和显式事务复制成本分别变化。未使用 Docker、2 核配额、cgroup 内存/IO 指标或全机清缓存；不是官方分数。

## 最终结果

以下均为三轮中位数，单位秒；写入窗口从第一条预生成请求发送至最后一条成功响应，含 JSONL 传输与解析，显式事务组含 BEGIN/INSERT/COMMIT。

| 工作负载 | 修改前写入 | 修改后写入 | 耗时下降 | 前写入＋关闭 | 后写入＋关闭 |
|---|---:|---:|---:|---:|---:|
| 10,000 × 128，B100，auto | 1.268626 | 0.305819 | 75.9% | 1.415410 | 0.445957 |
| 10,000 × 128，B100，explicit | 1.606817 | 0.514390 | 68.0% | 1.771487 | 0.658987 |
| 4,096 × 1024，B256，auto | 0.484147 | 0.330841 | 31.7% | 0.695370 | 0.526571 |

进程 CPU 秒与写入结束峰值工作集（不是 cgroup 峰值；Windows CPU 时间有计时粒度）：

| 工作负载 | 前 CPU s | 后 CPU s | 前峰值 MiB | 后峰值 MiB |
|---|---:|---:|---:|---:|
| auto | 0.3438 | 0.1875 | 21.96 | 21.90 |
| explicit | 0.6406 | 0.3750 | 26.57 | 24.13 |
| wide | 0.3125 | 0.2969 | 62.13 | 64.50 |

宽向量组的 CPU 时间和峰值内存没有全面改善，不能宣称所有资源指标都更好。每轮关闭后重启、逐条验证所有 id/category/content 和完整向量 float32 字节，共 144,576 行次。各组前后 WAL 与向量文件 SHA-256 完全一致；原始记录保留请求与二进制哈希、逐轮耗时和文件大小。

## 阶段诊断

诊断用 bench/build_write_profile.py 生成独立源码；发布运行时没有写入计时开关或计时开销。下表为额外一次 10k/128/B100 自动提交诊断，累计毫秒，包括两条建表/索引请求，非最终性能数字。阶段时钟只在请求及持久化阶段边界调用。

| 阶段 | 基线 | 中间候选：仍每次 stat WAL | 最终候选 |
|---|---:|---:|---:|
| sql_parse | 13.852 | 14.000 | 12.826 |
| prepare | 1.216 | 1.208 | 1.136 |
| apply | 78.764 | 79.214 | 77.118 |
| wal | 37.626 | 12.339 | 12.348 |
| vector_write_map | 10.106 | 4.429 | 4.110 |
| sync | 1024.570 | 144.583 | 153.302 |
| lsm | 25.714 | 847.615 | 1.691 |

`prepare` 包含 BEGIN 快照和语句准备；`apply` 包含向量解析、内存行、普通列索引与待提交记录维护；`wal` 包含编码和文件写入；`vector_write_map` 是向量写入及旧实现的映射调用；`sync` 包含同步及旧实现周边打开/关闭；`lsm` 包含 MemTable 维护、阈值检查和必要刷盘。

第一轮候选消除了重复表复制、CRC 位循环和 WAL 重复打开后，默认写入约改善 12%，显式事务约 16%。其 lsm 阶段仍约 0.8 s，定位到每提交 `fsize(wal)`。最终只在打开/恢复时取得长度，其余增量维护后，该阶段开销显著下降。具体 Windows 内核为何让这些文件状态操作昂贵仍未做内核剖析；这里陈述的是代码路径与对照观察，不把它误称为 Linux 的已知磁盘瓶颈。

## 正确性与持久化检查

- 最终候选：20 个原公开 SQL/断言通过（Windows 线程管道适配，不是 Docker 准入）。
- 最终候选：1470 次 scan/block/auto 独立真值检查通过，含更新、删除、显式事务查询后回滚和 checkpoint 重启。
- 最终候选：B256/B1024 × 有/无 checkpoint 的 4 组强杀恢复通过，核对普通字段与非零向量距离；SQL 引号、修改、回滚、强杀及 checkpoint 恢复 2 组通过。
- 最终候选：13 项 CRC/提交/事务检查通过。包含 176 个不同长度及非对齐缓冲区与独立逐位 CRC 比较、标准答案；短写、write EINTR、sync EINTR、向量/WAL 部分写、同步失败、提交后 LSM 失败、向量或 WAL 同步后退出、同事务失败 INSERT 后继续提交。12 个数据场景均由新进程检查恢复结果。
- 失败提交后验证 SELECT、ROLLBACK、COMMIT、checkpoint 被拒绝，避免返回未持久化状态或截断可能已提交的向量。WAL/MemTable 字节计数与真实大小核对，并检查 checkpoint/flush 清零。
- 上一轮 Manifest 发布/回收的 9 项故障恢复测试在本轮中间候选再次通过；最终增量仅调整 WAL 长度和 checkpoint WAL 同步，未改 Manifest 实现。
- 原有 2000 行检查仍失败：当前 2265 行，上一轮 2230 行。没有压缩源码规避。
- 这些是进程退出与注入错误检查，不是断电或完整磁盘满矩阵；STATE/索引发布等其他持久化路径仍有历史限制，未宣布整个存储引擎完成。ANN 召回问题也未改变。

## Linux 复现与提测

```sh
git show edde4ac:competition_lite/src/minivectordb_lite.cpp > /tmp/lite-before-write.cpp
g++ -O3 -DNDEBUG -std=c++17 /tmp/lite-before-write.cpp -o /tmp/lite-before-write
g++ -O3 -DNDEBUG -std=c++17 competition_lite/src/minivectordb_lite.cpp -o /tmp/lite-after-write
python3 competition_lite/bench/write_pairs.py --binary before=/tmp/lite-before-write --binary after=/tmp/lite-after-write --root /tmp/write-auto-new --rows 10000 --dim 128 --batch 100 --repeat 3
python3 competition_lite/bench/write_pairs.py --binary before=/tmp/lite-before-write --binary after=/tmp/lite-after-write --root /tmp/write-tx-new --rows 10000 --dim 128 --batch 100 --transaction explicit --repeat 3
python3 competition_lite/bench/write_pairs.py --binary before=/tmp/lite-before-write --binary after=/tmp/lite-after-write --root /tmp/write-wide-new --rows 4096 --dim 1024 --batch 256 --repeat 3
g++ -O1 -std=c++17 competition_lite/bench/write_commit_check.cpp -Wl,--wrap=write -Wl,--wrap=fdatasync -o /tmp/write-commit-check
python3 competition_lite/bench/write_commit_check.py --binary /tmp/write-commit-check --root /tmp/write-fault-new
docker build -t lite-write .
python3 protocol_check/run.py --image lite-write --output /tmp/public-write-new
```

benchmark 需要 numpy，输出目录必须是新目录。发布运行时不依赖 numpy。根 Dockerfile 仍编译 Lite。服务器暂不可用，Linux/Docker 与官方资源和数据条件下的收益须由下一次提测确认。原始结果见 [write_paths_20261114](../results/write_paths_20261114/)。
