# IK dispatch and TOKENIZE scratch memory

The previous platform submission, `f60d5acd352727e244c8eb6e195e13f1325be190`,
scored 42.466/50. Local diagnostic timers on that revision attributed about
half of IK expression evaluation to getting tokens, including the character
loop. BENG spent about 13% of expression evaluation on parser-name handling.
These are inclusive elapsed times from an instrumented build, so they identify
where to investigate and do not predict platform speedups.

The four IK segmenters are fixed at initialization. Store typed pointers and
invoke them directly in letter, quantifier, CJK, surrogate order. Every
segmenter still sees every character, including boundaries that flush tokens.
An initialization failure releases any constructed objects and clears their
pointers. This change also removes four list-node allocations per parser.

`TOKENIZE` puts the serialized parser name in a fixed local buffer and allocates
property strings in the expression's existing scratch arena. The properties
remain alive through tokenization and result packing. Parser name and property
validation, dictionary loading, and the output formats remain the same.

## Local validation

Two Windows native incremental builds, fresh stores, same four logical CPUs
and memory configuration. The database data and SQL were taken from the
unchanged benchmark. Three alternating construction pairs used 20,000 rows
and batches of 500. Six alternating query and tokenization pairs used 30
warmups, 200 calls per query, and 3,000 calls per tokenizer. A focused pass
used eight alternating pairs with 10,000 calls per tokenizer.

| Focused pass | Control mean (ms) | Candidate mean (ms) | Median paired reduction | Faster pairs |
| --- | ---: | ---: | ---: | ---: |
| IK | 0.12128 | 0.10557 | 3.95% | 7/8 |
| BENG | 0.06253 | 0.05773 | 4.80% | 8/8 |
| SELECT 1 | 0.01874 | 0.01902 | -0.95% | 4/8 |

A slow control sample raised the mean speedup, especially for IK. The paired
medians better represent this run. Construction mean elapsed times changed
from 8.239 to 7.747 s (IK two columns), 6.270 to 6.205 s (IK body), and 3.745
to 3.746 s (BENG). The four query timings varied across pairs; no reliable
query speedup is attributed to this patch.

Both versions matched on 112 TOKENIZE statements or edge cases and 200
full-text membership comparisons across four parsers. Multirow tests with
varying parser names and options, SMART and MAX_WORD, and prepared statements
reused after an error passed. The four benchmark queries matched on document
IDs and scores, as well as the actual LIMIT rows. The candidate also passed
large-document batch-boundary and concurrent DDL tests.

[Raw timings and result checks](fts-tokenizer-dispatch-local-results.json)
include all samples and the compiled source hashes. The Windows comparisons
are not official Linux benchmark results; the platform result is pending.
