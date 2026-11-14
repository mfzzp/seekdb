# exact 路径分离与本地对照：2026-11-14

本轮基于远端 `2026-competition-round2-lite2000` 的 `f885c285606e9c0b62806d92f29b96977e2badf8`。默认 exact 改为直接扫描，并保留显式 Block 对照路径。根 Dockerfile / .dockerignore 已切换到 Lite 源码，避免提测仍然编译根目录旧实现。

## 已确认的多余开销

1. 原来过滤候选超过 2048 后仍计算全表块下界、遍历全表块，再通过哈希集合和过滤检查挑出候选。本轮合成数据上，Block 的完整距离计算数与直接扫描相同，剪掉的块数为零。
2. 原关闭 Block 的路径按物理偏移排序，每次比较查找行表，并收集全部命中后再选 Top-K。两条新路径共用过滤后的物理位置、已有 SIMD 和有界堆；只排序整数位置，不在比较器中查行。
3. 原 SELECT 在知道候选是否为空之前调用 ensure_map，把 256 MiB 映射窗口切回文件开头。现在按需读取才映射，空候选没有这项成本。
4. 回滚重建普通列索引时统一使位置、矩阵和块缓存失效；截断回滚向量前解除映射并检查截断返回值。多列向量切换时检查块索引所属列及维数。

## 最终计时

Windows / MSYS2 GCC 16.2.0，Intel i5-1135G7；两版相同 `-D_GNU_SOURCE -O3 -DNDEBUG -std=c++17`。三轮串行运行，轮换 baseline/scan/block 顺序；最终运行期间没有并行编译或回归。每个进程从相同 checkpoint 启动，相同预热、相同 JSONL SQL，分别 1000 / 180 条查询。使用自建高斯 float32 数据和均匀类别，不是官方或先前服务器的生成器。

下表是各轮分组平均耗时的三轮中位数，单位 ms/请求，包含 JSONL 传输和响应解析。首条查询另存于原始记录，统计计时关闭。不是服务器速度、cgroup 资源结果或正式得分。

| 数据 | 条件 | 原版 | scan（默认） | block | scan 耗时下降 |
|---|---|---:|---:|---:|---:|
| 10,000 × 128 | none | 0.6987 | 0.6591 | 0.7047 | 5.7% |
| 10,000 × 128 | category | 0.3821 | 0.3077 | 0.3551 | 19.5% |
| 100,000 × 1024 | none | 195.9442 | 176.2140 | 181.8060 | 10.1% |
| 100,000 × 1024 | category | 43.7623 | 37.6558 | 40.9164 | 14.0% |
| 100,000 × 1024 | tenant | 48.5699 | 41.6225 | 45.2797 | 14.3% |
| 100,000 × 1024 | joint | 20.8793 | 15.9146 | 16.8994 | 23.8% |
| 100,000 × 1024 | range | 132.0530 | 109.4930 | 113.8996 | 17.1% |
| 100,000 × 1024 | empty | 9.1548 | 0.7378 | 0.7710 | 91.9% |

内存为查询进程峰值工作集，三轮中位数；不含装载进程，也不等于 Linux cgroup 峰值。
- 10,000 × 128：原版 24.42 MiB，scan 24.42 MiB，block 24.42 MiB。
- 100,000 × 1024：原版 342.44 MiB，scan 343.41 MiB，block 343.44 MiB。

## 单独诊断运行

阶段计时仅在 `LITE_PROFILE=1` 开启，循环按最多 256 条一批计时，不对每个向量读取时钟。`prepare` 包括查询准备、候选过滤和位置排序；`vector_distance` 合并向量访问与距离算术，不能据此将 mmap 成本和 SIMD 算术分别归因。`topk` 为堆维护；`result` 为最终排序、投影；`select_total` 不包含外层 JSON 编解码。`scan_total` 包含距离和 Top-K，不能再次相加。

10 万行 / 1024 维 warm profile 的分组中位数（ms；仅诊断，不作为上面的最终性能值）：

| 路径 | 条件 | 过滤后候选 | 完整距离数 | 准备 | 下界 | 距离及访问 | Top-K | 结果 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| scan | none | 100000 | 100000 | 0.5649 | 0.0000 | 173.8896 | 0.1989 | 0.0252 |
| scan | category | 5000 | 5000 | 1.5759 | 0.0000 | 35.2182 | 0.0185 | 0.0249 |
| scan | tenant | 14286 | 14286 | 2.8373 | 0.0000 | 37.1741 | 0.0347 | 0.0249 |
| scan | joint | 715 | 715 | 1.3688 | 0.0000 | 13.5523 | 0.0107 | 0.0255 |
| scan | range | 50000 | 50000 | 15.6729 | 0.0000 | 92.8498 | 0.1044 | 0.0244 |
| scan | empty | 0 | 0 | 0.3824 | 0.0000 | 0.0000 | 0.0000 | 0.0084 |
| block | none | 100000 | 100000 | 0.5653 | 2.1461 | 173.8660 | 0.2784 | 0.0250 |
| block | category | 5000 | 5000 | 1.5717 | 2.0349 | 35.7304 | 0.1087 | 0.0278 |
| block | tenant | 14286 | 14286 | 3.0653 | 2.3372 | 39.2129 | 0.1303 | 0.0258 |
| block | joint | 715 | 715 | 1.3923 | 1.2599 | 13.7372 | 0.0495 | 0.0243 |
| block | range | 50000 | 50000 | 16.0876 | 1.0981 | 92.6638 | 0.1508 | 0.0241 |
| block | empty | 0 | 0 | 0.3653 | 0.0000 | 0.0000 | 0.0000 | 0.0085 |

两条路径的这些随机数据查询均没有剪枝收益。已按此证据将默认路径设为 scan；`auto` 目前明确等同 scan，并非自适应优化器。Block 路径仅为有序/聚类数据保留，聚类正确性回归确认它确实能减少距离计算。大规模无过滤剩余成本集中于向量访问和距离阶段；现有 256 MiB 窗口在 409.6 MB 向量文件上发生重映射，Linux 上的页错误/映射成本仍需服务器剖析。没有把该现象直接归咎于 SIMD，也没有提高默认内存预算。

## 开关和语义

- `LITE_EXACT_PATH=scan|block|auto`：作用于升序 L2 + LIMIT 的 exact Top-K；其他排序保留通用语义。默认 auto=scan。
- `LITE_PROFILE=1`：启用 metrics.profile_ms；正式计时和默认镜像均不设置。
- `LITE_BLOCK_ORDER=bound`：显式 Block 路径按下界排序；默认按物理块访问。
- 旧 `LITE_BLOCK_EXACT=0/1` 兼容保留；显式 `LITE_EXACT_PATH` 优先。新两条路径都使用相同完整 SIMD 距离，不启用旧 `LITE_EARLY` 部分距离实现，避免两者精度/算术不一致。
- metrics 保留 planner_path=exact_scan 兼容既有协议，新增 exact_path、filtered_candidates、full_distance_evaluations、blocks_considered、blocks_pruned、block_pruned_candidates、heap_updates。candidate_count 仍为最小单索引候选，不等于全部过滤后的行数。
- 直接扫描在 SELECT 中不构建或访问块下界；checkpoint 原有的块统计持久化仍保留。Block 只为有候选的块计算下界。exact 的球下界因 float 半径没有向外舍入而停用，盒下界扣除保守浮点余量，严格大于当前最差距离才剪枝，保留同距离按 id 排序。

## Manifest 小修补和验证

新 SST/sidecar 写完并 fsync → 新 Manifest 写完并 fsync → 同步目录中的新文件 → rename 发布 Manifest → 切换内存视图 → 再次同步目录 → 删除旧 SST/sidecar。发布前失败不切换旧视图，也不删旧文件；rename 后目录同步失败时保留全部旧文件，兼容重启观察到任一版本。当前单写者不存在并发 SST 读引用。失败可能留下未引用文件，清理垃圾不属于本轮重构。关键写入、关闭、同步、rename 和 compaction 输入读取错误均检查。

- 1470 次独立真值查询通过：scan/block/auto、矩阵缓存开关、两列不同维数向量、并列距离、空/等值/范围过滤、LIMIT 0/1/10/超出行数、ASC/DESC、写后读取、事务内查询后回滚、checkpoint 重启。
- 10620 次计时查询的新旧/双路径完整响应 rows 相同；每个分组前三条另用 numpy float64 独立暴力校验 ID、顺序和距离容差。
- 9 个 Manifest 用例通过并逐一重启检查引用文件：成功、SST 写失败、Manifest 写失败、rename 失败、文件 fsync 失败、发布前/后目录 fsync 失败、发布前/后进程退出。故障通过独立链接包装器注入，运行时代码没有故障开关。这不等同断电试验。
- 原公开 20 个用例全部通过。原 runner 的 POSIX selector/killpg 无法在 Windows 管道工作，使用 bench/public_portable_check.py 的有界线程管道传输；原 SQL、期望结果和 assert_response/check_case 未修改。
- 原有 2000 物理行检查仍失败：本轮 2230，基线 2185；没有压缩代码规避。
- 本机没有 Linux/WSL/Docker，尚未构建实际提交镜像。根构建入口已修正，需要这次提测确认 GCC 12/Linux 和资源限制下的结果。旧报告的 ANN 召回问题、SST 主读路径、完整事务写集和其他持久化发布缺口仍待后续处理。

## 复现（Linux）

```sh
g++ -O3 -DNDEBUG -std=c++17 competition_lite/src/minivectordb_lite.cpp -o /tmp/lite-exact
git show f885c28:competition_lite/src/minivectordb_lite.cpp > /tmp/lite-before.cpp
g++ -O3 -DNDEBUG -std=c++17 /tmp/lite-before.cpp -o /tmp/lite-before
python3 competition_lite/bench/exact_paths_bench.py --baseline /tmp/lite-before --candidate /tmp/lite-exact --root /tmp/exact-10k-new --rows 10000 --dim 128 --queries 1000 --rounds 3
python3 competition_lite/bench/exact_paths_bench.py --baseline /tmp/lite-before --candidate /tmp/lite-exact --root /tmp/exact-100k-new --rows 100000 --dim 1024 --queries 180 --rounds 3
python3 competition_lite/bench/exact_paths_check.py --binary /tmp/lite-exact --root /tmp/exact-check-new
g++ -O1 -std=c++17 competition_lite/bench/manifest_publish_check.cpp -Wl,--wrap=rename -Wl,--wrap=fsync -o /tmp/manifest-check
python3 competition_lite/bench/manifest_publish_check.py --binary /tmp/manifest-check --root /tmp/manifest-check-new
docker build -t lite-exact .
python3 protocol_check/run.py --image lite-exact --output /tmp/public-lite-new
```

bench 数据生成/独立 oracle 需要 numpy。Windows 公开用例用 `python -X utf8 competition_lite/bench/public_portable_check.py --binary <exe> --root <新目录>`。所有输出目录要求新建，避免误用旧结果。

原始逐轮结果、分阶段诊断、二进制/源码/数据/查询哈希及回归摘要见 [exact_paths_20261114](../results/exact_paths_20261114/)。下一轮按提交号记录提测的功能失败项、exact/ANN 模式、查询耗时、写入耗时、内存和得分，再与本地候选数/距离数对照，不将一次反馈猜成官方隐藏数据规律。
