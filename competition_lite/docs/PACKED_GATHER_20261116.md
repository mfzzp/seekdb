# Packed screening with filtered gathers

VNNI-capable 128D matrices now build packed screening codes directly from the source floats. This removes the extra retained raw int8 copy and its transient allocation. Filtered queries gather the selected row's 32 four-byte groups into a 128-byte scratch buffer and use the existing integer distance kernel. Final ranking uses original float vectors and double distances. Unsupported CPUs and disabled VNNI retain raw codes. `LITE_PACKED_ONLY=0` restores dual-code storage for comparisons.

The first packed-only candidate computed whole 16-row blocks for filtered candidates. Its three-round local E median was 0.493487, so the gather implementation replaced that path. Both experiment records remain available. Two redundant timing/kernel comments were removed to keep all runtime sources at 2000 physical lines.

All work and results are under `/mnt/HDD2/zzp/oceanbase-competition/seekdb/.codex-lite2000-8afe25a`. The server restarted during the first gate. All final checks and paired runs were restarted after reboot. The paired baseline is local commit `7eae4396503558a3afdb9e072eea2494629d1514`, using its previously verified binary.

The frozen workload and harness are unchanged, with 10000 rows, 128 dimensions, batches of 100 committed rows, and 1000 Exact K10 queries split evenly between filtered and unfiltered. Each binary ran three times with alternating order, under 2 CPU and 2 GiB container limits. Physical I/O uses cgroup v2 io.stat and memory uses memory.peak, including writeback at shutdown. Official baseline and evaluation boundaries remain unavailable.

| Metric | Baseline median | Candidate median | Change |
|---|---:|---:|---:|
| Query interval seconds | 0.181156 | 0.180407 | -0.41% |
| Mixed request ms | 0.139392 | 0.138844 | -0.39% |
| Write seconds | 2.043887 | 1.853278 | -9.33% |
| Container peak MiB | 21.859375 | 20.730469 | -5.16% |

Paired local E values are 0.502088, 0.513200, 0.513292, with median **0.513200** against baseline self-comparison 0.5. Filtered request means increased from 0.118228 to 0.125883 ms. Unfiltered means decreased from 0.160630 to 0.152470 ms. The code change targets query cache storage. Observed write-time reductions are subject to HDD sync and scheduling variation and cannot establish a write-path speedup. The evidence supports a bounded memory improvement and a local composite gain, with a remaining filtered-query tradeoff.

The complete Docker gate passed. All six performance runs passed independent Exact verification and 10000-row restart verification. Additional screen-path checks passed 384 queries each with packed-only disabled and with VNNI screening disabled, covering ties, tails, noncontiguous candidates, updates, rollback and restart. Full gate, binary/input hashes, T/B/M snapshots, earlier failed candidate rounds and final paired reports are in [evidence.json](../results/packed_gather_20261116/evidence.json).
