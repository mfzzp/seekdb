# Agent collaboration evidence guide

Originally prepared by Codex against source revision `8f744ea` on 2026-11-11; integrated onto published revision `ac0c62fb05e7622d01d8b218e1483e528e760a1a` with explicit user authorization to push. The integration changes only agent material relative to that published revision. This is a retrospective guide to `conversation.json`, not a reconstructed verbatim transcript. Historical measurements remain attributed to their original executors and were not rerun during this material edit. Counts apply to the revision of each experiment.

## Problem and plan

The task combines SQL/protocol correctness, durable storage, filtered vector search and constrained resources. The working sequence was: establish a baseline, locate expensive work, test one hypothesis, compare outputs, and retain justified changes. Local performance is not a competition score.

## Attribution

The participant supplied teacher/team meeting notes, the bitmap proposal, server-directory constraints, iteration requests and delivery decisions. Original intellectual authorship beyond those supplied messages is not established. Codex generated the technical handoff later relayed to DeepSeek, including the cache-budget boundary, one-change-at-a-time rule and result.json verification requirement. Forwarding a prompt is participant action, but not independent technical authorship. Implementations and measurements belong to the named agents. No student-run experiments are claimed.

## Review trails

1. **Correct the diagnosis.** Read learning case `bitmap-proposal-to-verification`, experiment `bitmap-delete-comparison` and `tests/test_bitmap_delete.py`. Disk deletion already appended tombstones; retaining index snapshots was the actual optimization. Accessor counts include cache hits, not just disk I/O. Transfer: inspect where work occurs before optimizing the proposed layer.

2. **Correct the measurement.** Read experiments `scan-cache-admission` and `protocol-overhead-decomposition`. Cache-miss instrumentation was separated from timing; later decomposition challenged the earlier claim of fixed JSON overhead. `agent/scan-cache-benchmark.json` contains paired data. Transfer: ask for a control and stage timings, not just a speedup. Later DeepSeek numbers are reported historical evidence, not independently rerun here.

3. **Constrain numerical optimization.** Read `correctness-and-memory-risk-pass`, `float32-matrix-margin`, and `identity_error_margin` / `select_exact` in `minivectordb.py`. Algebraic rewrites change rounding. Check IDs, order and distance tolerance separately; finite tests do not prove universal bitwise equivalence. Transfer: choose an independent oracle and adversarial boundaries before accepting a faster kernel.

4. **Test environment discovery.** Read `budget-under-container-limits`, `cgroup-limit-reader` and `tests/test_cgroup_limit.py`. Root-path lookup missed a nested limit despite earlier injected-budget tests. Transfer: mocks do not prove deployment discovery. Real cgroup checks still do not replace image-build validation.

5. **Connect the latest published repair to evidence.** Read `cache-reuse-and-bit-order-20261111`, `tests/test_incremental_matrix.py` and `tests/results/cache-reuse-20261111.json`. The recorded change reuses unchanged matrix rows while accounting for rebuild allocations, and corrects NumPy deletion-bit interpretation. The attached benchmark is historical evidence from that published change, not a new timing run in this integration. It does not establish an approximate ANN index or a Docker build.

## Reproduction and limits

The machine-readable source is `agent/conversation.json`, referenced above by exact experiment ID. Repository evidence includes `tests/`, `agent/query-path-benchmark.json`, `agent/scan-cache-benchmark.json`, `agent/competition-benchmark.json` and `VERIFICATION.md`.

Historical server harnesses are under `/mnt/HDD2/zzp/round2-results/deepseek-r1-20261111`; old `scripts/...` references may be relative to that directory, not the repository. Check availability before reproducing them.

No hidden-case pass, image-build success, ANN recall guarantee or Agent Prompt score is claimed. Report skips separately from passes and separate local timings from official scores. Keep credentials out of evidence. New records should name their actual author and executor.

## A short review route

Start with `agent/LEARNING_MAP.json`: six cards connect the problem, correction or rejected hypothesis, a transferable method and an existing artifact. Each card includes the SHA-256 of its source experiment record (JSON sorted by key, UTF-8, Python json.dumps default separators), so later source edits can be detected. This hash checks record consistency, not the truth of a measurement or the identity of its executor.

The rejected write-path proposal matters: `write-path-probe-no-change` reports that no safe improvement was established. Retaining validation and commit durability is an explicit engineering decision, not a missing experiment. The float32 case also matters: `float32-matrix-margin` distinguishes an incorrect bound from attempted full-query reproductions that did not fail. Do not rewrite those unsuccessful attempts as successful bug demonstrations.

## How to read the evidence status

`LEARNING_MAP.json` separates historical results, current reference checks, rerun status and reproduction prerequisites for each card. Artifact hashes detect changes; they do not authenticate an experiment. All six experiment rerun statuses remain `not_rerun` during this documentation pass. The write probe was located on the server, but its original full workload was not executed here.

| Evidence | What it supports | What it does not support |
| --- | --- | --- |
| User quote or forwarded handoff | A stated request or constraint, with origin attribution | Independent technical authorship or mastery |
| Historical experiment entry | What the named agent recorded at that revision | A new measurement on the present checkout |
| Existing test source | An inspectable test design and assertions | A passing test execution in this audit |
| Hash and file check | Stable source record and available artifact | Correctness of the measurement |
| Material validation run | JSON, citations, document consistency and encoding checks | Database correctness, performance or evaluator score |

## Read the failed attempts as well as the gains

The `claim_notes` in the map explain ambiguous historical pass/skip counts and the distinction between an injected unit counterexample and an end-to-end failure. A test that manually supplies approximate distances demonstrates selection behavior, not that BLAS produced those same values in a real query. Historical phrasing remains in the experiment record; its scope is clarified here rather than silently upgraded.

## Continue with attributable evidence

For a future actual experiment, retain the prompt as sent, original author or forwarded origin, hypothesis, decision-changing evidence and final limitation. An unsent prompt suggestion is a proposal, not a past conversation. No participant reflection or successful experiment is invented by this guide.

Reviewer questions: Which assumption was challenged? What could disprove it? Which observation changed the decision? Were timings and pass/skip counts comparable? What transfers to another workload, and what remains machine- or revision-specific? These are review questions, not claims that the participant has already answered them.

## Final integration status

All 67 experiment entries from the published baseline are retained unchanged. `EVIDENCE_AUDIT.json` records this integration's material checks and verifies that tracked files outside `agent/` match the published baseline. Previous instructions to defer pushing remain historical records; the user explicitly authorized this final integration and push. The final commit ID is reported after Git verification rather than embedded inside its own content.
