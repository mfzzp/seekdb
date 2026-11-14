# MiniVectorDB 当前完整实现 Prompt（9b7617e4）

本文件是当前架构实现的执行 Prompt。旧的 `CPP_AGENT_PROMPT_7DE2665.md` 与
`CPP_AGENT_PROMPT_F6396AD.md` 只用于历史审计，不代表当前实现规范。

## 目标与边界

- 仓库：`https://github.com/mfzzp/seekdb`
- 当前实现分支：`codex/architecture-2d227d3`
- 竞赛镜像分支：`2026-competition-round2-cpp`
- 基线 commit：`9b7617e448523c09b5828ad58c833714787fc512`
- 服务器工作目录：`/mnt/HDD2/zzp/seekdb-cpp-integrated`
- 入口：`minivectordb.cpp`、`v2/v2_storage.cpp`、`Dockerfile`
- 协议：stdin/stdout JSONL；stdout 只能输出协议响应，诊断写 stderr。

本 Prompt 严格服从《MiniVectorDB 最终架构设计》26 节。设计文档优先于旧代码、
旧 Prompt、临时实验和未经验证的性能猜测。

## 硬约束

1. 只在 `/mnt/HDD2/zzp/seekdb-cpp-integrated` 修改、构建、提交、验证。
2. 修改前检查分支、HEAD、工作区和远程引用，不能覆盖未提交修改。
3. WAL、Manifest、快照、Vector Segment 和索引发布必须有可恢复顺序。
4. 事务使用私有 WriteSet 和 ReadView，不能以整张 `State` 副本作为主机制。
5. 新 WAL 使用二进制最小变化记录，不能继续把 SQL 文本作为事实格式。
6. Block、MemTable、PQ、缓存和索引是派生结构，不能替代原始行和 float32 向量。
7. exact 覆盖完整候选域并使用安全界；ANN 不能冒充 exact。
8. Block 必须按行顺序组织并保留全部 Block，不能只存一个 Block 或覆盖旧 Block。
9. 每个确认提交都有单调 commit_seq、闭合提交边界和可重放恢复结果。
10. 性能数字必须附带数据、命令、原始日志、正确性和比较基线。

## 26 节完成清单

### 01 成本模型
同时核算写放大、I/O、距离 CPU、内存、事务可见性和恢复成本，不能只优化一次 Top-K。

### 02 容量推导
以百万行、1024 维 float32 和有限内存为目标，按公式估算原始向量、元数据、PQ、索引和缓存；不能默认常驻全表向量矩阵。

### 03 数据身份与版本
实现稳定 RowId、VectorId、row_version、commit_seq、location_generation、index_generation；更新、删除、压缩不能破坏身份。

### 04 SQL 与命令路由
解析、绑定、类型校验、DDL、DML、事务、exact/ANN 分层；错误码稳定，stdout 纯净，shutdown 干净。

### 05 事务
私有 WriteSet、ReadView、读己之写、原子提交、失败回滚和冲突边界必须明确；禁止整张 State 复制。

### 06 WAL
二进制 WAL 具备版本、批次共享头、commit_seq、长度、CRC 和闭合提交边界；尾部撕裂可截断，中间完整损坏必须报错。

### 07 MemTable 与元数据 LSM
Active MemTable 有界，Immutable 可排队 flush；元数据 LSM 保存主键、标量、版本、墓碑和定位信息，不保存无界历史向量。

### 08 独立向量存储
原始 float32 进入独立 Vector Segment，按 VectorId/Location 局部读取；小字段更新不能反复搬运大向量。

### 09 Flush、Manifest、原子发布
不可变文件、Manifest/VersionSet、临时文件、fsync、rename、目录同步和失败恢复顺序必须完整。

### 10 标量索引与位图
等值/范围/category/倒排/删除位图按可见 row_version 更新；索引失效时回退合法扫描，不能漏刚提交数据。

### 11 ANN：IVF-PQ
使用 IVF-PQ 生成候选，原始 float32 做最终精排；nlist、nprobe、PQ、候选预算可配置并可回退。

### 12 增量可见性
刚提交的 MemTable 增量立即可查；基础索引、Immutable、Vector Segment 和增量候选共享同一 ReadView。

### 13 exact 执行器
覆盖完整候选域；Block、范围、中心/半径和采样维度只能提供保守安全下界；平局按主键稳定排序，按需读原值。

### 14 距离内核
支持 float32 L2 及允许的其他度量；SIMD、提前终止和缓存不能改变 exact 数值或排序契约。

### 15 查询计划器
依据过滤选择性、Top-K、维度、索引状态和成本选择扫描、Block exact、IVF-PQ ANN 或混合计划，并记录计划。

### 16 内存与缓存
统一核算 MemTable、Immutable、LSM、向量缓存、PQ、查询缓存、排序堆、allocator 和 OS 页缓存；缓存有界可淘汰。

### 17 后台工作与背压
flush、compaction、索引构建、向量整理、WAL 轮转、旧文件回收和失败重试不能无界阻塞协议线程；队列和背压可观测。

### 18 删除、旧版本与 Vector GC
删除先写墓碑并按 ReadView 生效；旧版本/不可达向量只能在安全水位后回收，不能删除仍被引用的文件。

### 19 恢复
恢复提交事实和文件集合版本；按 Manifest 水位回放 WAL，验证 CRC、序号、提交闭合，区分尾部撕裂和中间损坏。

### 20 端到端协同
用 CREATE、INSERT、UPDATE、DELETE、BEGIN、COMMIT、ROLLBACK、checkpoint、重启、exact、ANN、过滤和删除验证全生命周期。

### 21 对齐、顺序写与 I/O
明确 WAL、元数据、向量段、校验页、随机读取和顺序写的对齐单位；不能把逻辑记录大小称为设备原子写大小。

### 22 参数表与约束
集中定义 WAL frame、segment、MemTable、Immutable、Block、cache、nprobe、checkpoint、GC、背压参数，禁止散落魔数。

### 23 正确性不变量
验证主键唯一、身份稳定、提交单调、ReadView 一致、删除不可见、exact 不漏、ANN 不冒充 exact、Manifest 原子、恢复幂等、stdout 纯净。

### 24 模块边界与接口
SQL/协议、Transaction、WAL、MemTable/LSM、VectorStore、Manifest/VersionSet、ScalarIndex、IVF-PQ、ExactExecutor、AnnExecutor、Scheduler/GC 使用窄接口，禁止越权保存事实数据。

### 25 收益证明
每项优化使用相同硬件、编译参数、数据、查询集合和正确性校验，报告 P50/P95、RSS、启动/恢复时间和原始日志，不能只报最好值。

### 26 明确排除
排除整表 State 事务副本、SQL-only WAL、向量塞进元数据 LSM、默认全表矩阵、固定少量桶冒充 exact、单 Block、无界缓存、无水位 GC 和未经验证的性能倍数。

## 当前代码审计矩阵

每次修改都标记“已实现 / 部分实现 / 未实现”，不能把字段存在写成模块完成。重点核对：

- `tx = st` 是否仍是事务主路径；
- WAL 是否具备完整 commit marker/CRC/批次语义；
- MemTable、Immutable、LSM、IVF-PQ、后台 flush/compaction/GC/backpressure 是否真实运行；
- Vector Segment、Manifest、读视图、旧版本和文件回收是否闭合；
- exact 与 ANN 是否遵守不同执行契约；
- 旧快照和旧 WAL 的迁移是否有可复现实验证据。

## 必须执行的验证

```bash
cd /mnt/HDD2/zzp/seekdb-cpp-integrated
git status --short --branch
git log --oneline --decorate -5
git diff --check
g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra \
  -o /tmp/minivectordb-current minivectordb.cpp v2/v2_storage.cpp
python3 protocol_check/run.py \
  --command '["/tmp/minivectordb-current","--data-dir","{data_dir}"]' \
  --config protocol_check/config.json --output /tmp/public-current
docker build --no-cache -t seekdb-eval:<commit> .
printf '%s\n' '{"id":1,"op":"shutdown"}' | docker run --rm -i seekdb-eval:<commit>
```

10 万行测试至少记录：批量写入、进程内 exact/ANN P50/P95、checkpoint、重启首次查询、恢复时间、峰值 RSS、返回行数、错误数和 JSONL/stdout/stderr 原始路径。

## 提交和远程核对

```bash
git diff --check
git status --short --branch
git show --stat --oneline HEAD
git push origin codex/architecture-2d227d3
git push origin 2026-competition-round2-cpp
git ls-remote origin refs/heads/codex/architecture-2d227d3 refs/heads/2026-competition-round2-cpp
```

最终报告必须列出修改文件、commit、分支、远程 SHA、构建和协议结果、性能原始日志、正确性结果、未实现项和风险；未执行的测试必须明确标记。
