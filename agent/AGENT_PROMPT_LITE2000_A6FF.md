# Agent Prompt for MiniVectorDB Lite a6ff81a

## Scope

This prompt documents the work that produced the `competition_lite` runtime at commit `a6ff81af4d258da4221b4723f7e0493a250daf96`. The repository is `mfzzp/seekdb`, and the target branch is `2026-competition-round2-lite2000`. Work was performed in `/mnt/HDD2/zzp/oceanbase-competition/seekdb` under the user's server-directory constraint.

The task combines JSONL protocol correctness, Exact vector search, durable commits and restart recovery, filtered Top-K performance, and resource accounting. The evaluation target is the frozen 10000-row, 128-dimensional workload with 100-row write batches and 1000 Exact K10 queries, half with equality filtering.

This is a record of an actual engineering process. User supplied the task requirements, server location, performance rules, optimization direction, and delivery constraints. Codex performed the source inspection, server commands, implementation work, validation and measurements recorded in the linked artifacts. Historical agent material inherited from earlier commits remains historical and is not presented as a new experiment.

## Learning and planning

The first diagnosis separated end-to-end query time from engine stages and separated write time, physical I/O and container memory. The working hypothesis was that the existing int8 screening and Exact double path were more promising than ANN or persistence relaxation. The accepted plan was to preserve Exact distances, primary-key tie breaking, WAL ordering, rollback, restart recovery and full-vector durability.

The implementation sequence was deliberately incremental.

1. Inspect the tokenizer, query execution path, matrix screening path, scalar postings and commit path.
2. Keep an independent Exact oracle and run the repository's Docker gate before performance comparison.
3. Test one query change at a time and retain a change only after correctness and repeated measurements.
4. Compare the full T/B/M vector, since E includes elapsed time, physical I/O and peak container memory.
5. Record failed candidates and limitations together with retained improvements.

## Prompt design and iteration

The initial candidate used packed int8 screening and survivor batching. The stage profile showed that filtered queries still spent most of their time in Exact double distance evaluation. A packed-only experiment reduced memory but made filtered requests slower because non-contiguous rows required gather decoding. A continuous temporary decode experiment was then tested and rejected after three paired rounds because it reduced E. The retained `a6ff81a` path keeps the packed screening implementation that passed the complete gate.

The independent comparison also tested two later branches. `ec0ddfe5de591d68a9130917b99a3db3139fc955` bypasses screening for small filtered postings. `bb0ac19c94d71c0609cfd5540a6d1dad223c2c5c` uses a scalar survivor fast path. Both were measured with the same prepared input and Exact oracle. They were not silently substituted for a6ff.

## Verification and independent judgment

The final a6ff Docker gate passed build, public and extra protocol cases, Exact path checks, matrix and screening kernels, JSON input/output checks, update and rollback cases, LSM model and corruption checks, commit-fault checks and full-vector WAL recovery. Each paired performance run independently verified Exact answers and restart results. Recall for both filtered and unfiltered groups was 1.0 in the retained measurements.

The measurements are local evidence. Query wall time includes the repository harness's driver SQL generation, JSON transport and response handling. Physical I/O and memory use the harness snapshots. The official evaluator, official baselines and ranking are not available in this record. The observed physical I/O remained effectively unchanged across the compared versions. HDD write time varied between rounds, so a single faster write round is not treated as proof of a write-path improvement.

## Result and decision

The direct four-version comparison used `paired_compare.py`, the same prepared workload, the same Docker resource limits, three alternating rounds, and independent verification. The retained a6ff version had the best local composite result against the original `8afe25ae9a40ab4c60df2fed2415f80bb49b1e00` reference in that run.

The comparison artifacts are under `agent/evidence/a6ff-vs-8afe/summary.json` and the per-run JSON files in the same directory. The measured local E values were computed with 8afe as the within-run reference. They are not official contest scores.

## Reproduction commands

```bash
python3 competition_lite/tools/check_lines.py
python3 competition_lite/bench/docker_gate.py --source . --root /tmp/lite-gate-a6ff --image lite:a6ff
python3 competition_lite/bench/paired_compare.py \
  --prepared /mnt/HDD2/zzp/oceanbase-competition/seekdb/optimization-20261116/prepared10k \
  --oracle /mnt/HDD2/zzp/oceanbase-competition/seekdb/optimization-20261116/oracle10k.jsonl \
  --root /tmp/lite-paired-a6ff \
  --image lite:a6ff \
  --variant a6ff=/absolute/path/to/minivectordb-a6ff \
  --rounds 3
```

The exact binary paths and SHA-256 values used in the four-version comparison are recorded in the surrounding server comparison directory and in the per-run JSON reports. Re-running requires the same image, prepared inputs, resource limits and device accounting if a comparable T/B/M result is desired.

## Limitations and transfer

The result does not establish an official rank, an ANN recall claim, a universal speedup, or a physical-I/O reduction. The filtered path remains sensitive to non-contiguous vector access and HDD scheduling. The transferable method is to identify the measured stage, preserve an independent Exact oracle, compare all three resource dimensions, and reject changes whose local composite result or correctness evidence does not hold.

No credential, token, password or private URL is included in this prompt or its evidence files.
