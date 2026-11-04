# AI Column architecture audit and DDL implementation boundary

This document records the initial audit. Current implementation and validation
status are in [ai-column-ddl-implementation.md](ai-column-ddl-implementation.md).

Baseline: 5cc1ef772effd5f72689c2d298d622a34319af4c (confirmed 110-point submission).
Branch: competition-2026-ai-column-ddl. Original worktree and old AI Column branch are preserved.

## Existing implementation and actual tests

The actual five mysqltest suites are under tools/deploy/mysql_test/test_suite/ai_column,
not under ai_function. The mock and result files are present. The three requested C++
unit-test files are absent, and this baseline has no unittest/CMakeLists.txt. CMake
conditionally adds that directory only when the file exists; adding a test source
alone does not create a runnable unit-test target.

Parser: src/sql/parser/sql_parser_mysql_mode.y already has the AI keyword and SQL
expressions for AI functions, but no AI COLUMN attribute. Ordinary column definitions
have four children; generated columns have seven. CREATE and ALTER share column
attribute resolution in src/sql/resolver/ddl/ob_ddl_resolver.cpp. Do not change those
node layouts or treat the new clause as an ordinary ignored attribute.

DDL flow: ob_create_table_resolver.cpp / ob_alter_table_resolver.cpp -> their statement
arguments -> src/sql/engine/cmd/ob_table_executor.cpp -> src/rootserver/ob_ddl_service.cpp
and ob_ddl_operator.cpp -> src/share/schema/ob_table_sql_service.cpp. DDL metadata
writes must use the existing transaction. ALTER MODIFY also copies individual column
properties in ob_ddl_service.cpp; extending only assign() is insufficient.

Schema: src/share/schema/ob_column_schema.{h,cpp} owns the column data and implements
reset, assign, deep-copy sizing, serialization and deserialization. Table schema lives
in src/share/schema/ob_table_schema.{h,cpp}. SQL loading is in
src/observer/schema/ob_schema_retrieve_utils.ipp. Inner tables are generated from
src/share/inner_table/ob_inner_table_schema_def.py by the generator used in top-level
CMakeLists.txt. The generated sources are build outputs. Column flags above bit 31
are not stored in __all_column; extended_type_info already belongs to enum/set/vector
types and cannot be repurposed for AI expressions.

AI calls: src/sql/engine/expr/ob_expr_ai/ob_expr_ai_complete.cpp and
ob_expr_ai_embed.cpp use the current AI service/model implementation. The endpoint
interface is src/query/api/query/ai/ob_ai_endpoint_resolver.h. AI_EMBED is marked
NOT_VALID_FOR_GENERATED_COL. Reclassifying it as a generated-column function would
introduce model requests in the user transaction; keep the existing synchronous AI
function semantics unchanged.

DML: src/sql/engine/dml/ob_table_{insert,update,delete}_op.cpp use native DML paths.
Transactions end through src/sql/ob_sql_trans_control.cpp. Transaction callbacks in
src/storage/tx/ob_trans_end_trans_callback.* are not a durable work queue. Scheduler
persistence exists in src/observer/dbms_scheduler/ob_dbms_sched_table_operator.*.
A callback may wake a worker, but committed task-table rows must remain authoritative.

Vectors: existing schema checks are in ob_ddl_resolver.cpp; index DDL and native
maintenance live under src/storage/ddl and src/observer/vector_index. Async publication
must issue a normal transactional data modification so index maintenance remains native.
No ai_column_strict_read implementation was found in the current system-variable code.

## DDL contract established by the provided tests

ai_column_ddl.test requires __all_ai_column rows containing column_name, max_retries,
normalized_expr and three hidden-column identifiers. Each result column is visible;
state, request, and result-spec columns are hidden. A JSON completion expression must
persist the type instruction `Return exactly one valid JSON value`. ALTER ADD must
persist metadata. An embedding dimension mismatch must return 7600, and an AI-column
dependency on another AI column must return 1235. Do not change these expected results.
This suite makes no model HTTP requests.

## Metadata design and dependencies

A definition is keyed by owning table_id and result column_id, with a monotonic
schema/definition version, normalized expression, resolved dependency column IDs,
model identity/type, async policy and retry budget. The result type remains in the
native result-column schema, including vector extended type information. Store no
endpoint access keys in AI Column metadata.

The initial implementation step added the native __all_ai_column catalog (1131) and
its history (1132); both IDs were unused in this baseline's definition source.
The subsequent Parser/Resolver, hidden-column and persistence implementation is
described in ai-column-ddl-implementation.md. No passing Linux result is available.

The original integration plan was to add an allocator-owned definition to column/table schema, including
all copy, serialization, SQL store/load and ALTER paths. A separate catalog transaction
would violate DDL atomicity. Validate CREATE expressions only after all columns are
bound, resolve dependencies to IDs, reject missing/self/AI dependencies and unsupported
expression forms, validate model category without calling the model, and check vector
dimensions. JSON prompt enrichment belongs to a normalized executable expression,
not a test-only metadata string. Allocate hidden IDs using native table column-ID
allocation, check the maximum-column limit, and persist the hidden definitions and AI
catalog rows in one DDL transaction. SHOW AI COLUMNS must use the same metadata and
normal name/privilege resolution. ALTER MODIFY/DROP must invalidate definition versions
and prevent orphaned catalog rows.

Only after that DDL slice passes should DML enqueue tasks in the user transaction.
Worker reads must exclude uncommitted rows; rollback must remove eligibility with the
transaction. Snapshot the definition version, persisted request token, source row
version, and evaluated input per generation. Claim/lease tasks transactionally, perform
HTTP outside that transaction, and recheck the versions before writing. Publish all
AI columns of one row generation in a single transaction after every member reaches a
terminal state, allowing partial FAILED outcomes. A restart or commit ambiguity must
not turn an old response into a current one. Do not use process-memory counters for
request identity or ordinary result NULL values for readiness.

## Validation and remaining risks

The catalog change alone does not enable the DDL tests. Existing tests and expected
results are untouched. Windows has no installed Python interpreter according to
py -0p, no Linux environment has been established, and no C++ compilation, schema
code generation or mysqltest result is claimed. Validate generation using the native
generator into a temporary output tree, then build on Ubuntu 24.04 with the official
toolchain/dependencies. A newly added system catalog needs fresh bootstrap validation;
a dirty existing database directory is not evidence of successful catalog creation.

Linux commands for the current foundation:

    python3 unittest/share/inner_table/test_ai_column_catalog.py
    ./build.sh release --init --make -j"$(nproc)"

The actual runner is .github/script/seekdb/mysqltest_for_seekdb.py; run `run --help`
for case selection and pass the built seekdb, obclient/mysqltest runtime, and isolated
base/work directories as .github/script/seekdb/mysqltest_slice.sh does. The target DDL
case is ai_column.ai_column_ddl, and original regression cases include ai_function and
file_sql. Do not claim these suites pass until the complete DDL integration is present
and the runner has executed them.
