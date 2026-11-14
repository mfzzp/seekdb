# 正确性失败排查：2026-11-15

## 平台反馈和当前结论

用户确认提测提交为 a3615e8a5df3326aa01f6387f2fb0d1d873ea782。平台反馈：功能结果不符合要求 4 项、响应 id 不一致 1 项、响应前退出 1 项；未执行性能评测。没有拿到对应的具体输入、用例名和 stderr，不能把下列本地复现声称为官方 6 项的一一对应根因。

这不是已经证明的服务器环境问题。在原提交的本地二进制上可以独立复现协议和 SQL 缺陷。此前 20 项公开用例通过是真实的，但没有覆盖这些边界；它们不等于正式 29 项准入。f885c28 的根 Dockerfile 使用旧 minivectordb.cpp，而本次使用 Lite，旧实现已取得分数不能证明新实现已准入。

用户每天只有两次正式提测机会。此前在已知 ANN 召回不合格、实际 Linux/Docker 未验收时推荐 a3615e8 提测，属于交付把关不足。当前修复只是保存待验证代码，不建议再消耗正式提测机会。服务器可用后必须验证实际镜像，而不是只运行另外编译的二进制。

## 已复现和修复的问题

| 路径 | 原版最小现象 | 修复 |
|---|---|---|
| 通用异常响应 | SELECT 不存在的表，返回 error 但没有请求 id | 通用异常也回传原类型 id，并将异常原因写 stderr |
| SQL 越界 | 单独 INSERT 触发无响应退出 | 在访问 token、表和列之前检查长度；错误请求后会话继续可用 |
| JSON 字符串 | Unicode 转义被当作普通字符，字符串 id 也被改写 | 解码 Unicode 及代理对；输出转义所有控制字符 |
| 标量排序 | 超过 2^53 的 BIGINT 经 double 比较混淆；VARCHAR 没有比较值 | 按列类型比较 int64 或字符串，保留 id 并列规则 |
| 过滤 | != 条件没有排除相等记录 | 实现 != / <>，拒绝未知比较符 |
| 类型校验 | VECTOR(0)、错误 BIGINT、超长 VARCHAR 可进入状态 | 正长度校验、完整整数解析、Unicode 字符数校验；规范整数索引键 |
| 向量文本 | [1 2] 等缺少分隔符的输入被接受 | 检查分隔符、空元素和尾逗号；标准数值转换避免手写指数溢出 |
| 距离精度 | 128 维的 [10000, ..., 第16维=1] 与第16维=0 的距离合并成相等 | float32 存储不变，先转 double 再相减和累加，保留 SIMD |
| 距离范围 | 合法的 1e20 坐标在 float32 平方时溢出，输出无效 JSON | double 距离算术，非有限输出明确报错 |
| 投影 | SELECT distance,id,title 返回 id,title,distance | 保持 SELECT 的列位置 |
| 事务 DDL | 回滚表后遗留索引名，无法重建同名索引 | 事务保存和恢复索引元数据，清除回滚对象缓存；成功提交释放旧快照 |
| ANN | 512×32 独立随机数据中首次无过滤查询 Recall@10=0.7 | 全部 ANN 请求暂时走精确执行器，明确 ann_fallback=true |

DDL 回滚沿用本实现既有的事务 DDL 选择，不能据此推断官方某个未公开用例采用了相同输入。ANN 回退适用于所有数据，不识别测试集、不缓存结果；IVF/PQ 本身尚未修好，不再默认用于丢弃候选。

## 验证结果和范围

Windows/MSYS2 GCC 16.2，最终候选使用 -D_GNU_SOURCE -O3 -DNDEBUG -std=c++17。没有 Docker、WSL/Linux 内核、官方数据或评分程序。

- 新增 admission_edges_check.py 共 13 组；原 a3615e8 二进制失败 12 组（强杀恢复组通过），修复后 13/13 通过。保留两侧原始结果。
- 原公开用例 20/20 通过；使用原 SQL、原断言和线程管道适配器，不冒充 Docker 检查。
- 原 exact 路径回归 1470 次通过，覆盖 scan/block/auto、缓存切换、过滤、事务、更新删除、重启。
- 原提交故障与 CRC 检查 13 项通过，包含 12 个独立进程恢复检查。
- B256/B1024 × 有/无 checkpoint 的 4 组强杀恢复通过；字符串 WAL 重放与 checkpoint 两组通过。
- 源码和候选二进制哈希、统计、原始结果位于 results/admission_20261115。

这些检查不证明平台 6 项已全部解决。新增测试中的 case 名是本地命名，不是官方失败用例名。退出复现捕获的是 EOF，现有线程适配器可能在进程退出码可见前记录 null；不将该字段伪造为已知信号。

本轮没有重新声明性能收益。double 距离算术、严格解析和 ANN 精确回退可能增加耗时，必须在正确性通过后重测。2311 个物理行仍超过原有 2000 行检查。全量内存行表、完整事务写集和其他未完成持久化路径仍有限制。

## 服务器实际镜像验收步骤

在仓库根目录执行，OUT 必须是新目录；整个目录用于保存报告，失败也要保留 stderr。

```sh
OUT=$(mktemp -d /tmp/lite-admission-XXXXXX)
git rev-parse HEAD > "$OUT/commit.txt"
docker build --no-cache -t lite-admission .
docker image inspect lite-admission > "$OUT/image.json"
python3 protocol_check/run.py --image lite-admission --output "$OUT/public"
docker run --rm --cpus=2 --memory=2g --memory-swap=2g --pids-limit=128 \
  --network=none --read-only --cap-drop=ALL --security-opt=no-new-privileges \
  --mount "type=bind,src=$PWD,dst=/checks,readonly" \
  --mount "type=bind,src=$OUT,dst=/data" \
  --entrypoint python3 lite-admission -B /checks/competition_lite/bench/admission_edges_check.py \
  --binary /app/minivectordb --root /data/edges
docker run --rm --cpus=2 --memory=2g --memory-swap=2g --pids-limit=128 \
  --network=none --read-only --cap-drop=ALL --security-opt=no-new-privileges \
  --mount "type=bind,src=$PWD,dst=/checks,readonly" \
  --mount "type=bind,src=$OUT,dst=/data" \
  --entrypoint python3 lite-admission -B /checks/competition_lite/bench/exact_paths_check.py \
  --binary /app/minivectordb --root /data/exact
```

以上命令尚未在本机执行。还需在服务器运行原有资源受限与恢复检查，检查退出状态及日志，再评估是否值得下一次正式提测；不能因为这些自测通过就保证正式 29 项全部通过。
