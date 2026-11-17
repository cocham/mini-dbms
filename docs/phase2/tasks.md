# Phase 2 구현 순서

## 현재 상태

**Phase 2 완료(2026-11-16).** P2-01~P2-05 모두 구현·검증·커밋. Phase 3는 시작하지 않는다.

진행 방식(2026-11-16 변경, DB-P2-D10): 한 단위 구현 → 단위·CLI 테스트와 메모리 검사로 검증 → 통과하면 바로 커밋하고 다음 단위. 사용자 동작이나 공개 인터페이스를 바꾸는 결정은 구현 전에 짧게 제안한다. 학습은 사용자의 별도 노트로 진행하며 진행 조건이 아니다. Phase 3는 시작하지 않는다.

| ID | 기능 | 완료 기준 | 상태 |
|---|---|---|---|
| P2-00 | 구현 전 결정 | design.md DB-P2-D01~D08 | 완료(2026-11-15) |
| P2-01 | id 조회 최소 경로 | B+ 트리 삽입·분할·탐색, 시작 시 인덱스 구축, INSERT 시 등록, `WHERE id = ?`가 트리 → 위치 → 한 줄. spec B1~B4 | 완료(커밋 f331be6) |
| P2-02 | EXPLAIN 실행 계획 | `EXPLAIN SELECT ...`가 INDEX LOOKUP / FULL SCAN과 계획 시점 통계(트리 높이, 전체 행 수)를 보여 줌. spec B5 | 완료 |
| P2-03 | id 범위 조회 | `BETWEEN`, `<` `<=` `>` `>=`를 리프 연결로 처리, EXPLAIN에 INDEX RANGE SCAN. spec B6 | 완료 |
| P2-04 | 100만 건과 성능 비교 | 대량 SQL 생성, 시작 비용과 조회 시간 분리 측정, 인덱스 vs 선형 탐색 결과 기록. spec B7 | 완료 |
| P2-05 | 전체 검증·마무리 | Phase 1 회귀, 메모리 검사, 문서 정리. spec B8 | 완료 |

기능마다 하는 일: (인터페이스 변경 제안) → 구현 → 단위·CLI 테스트 → 메모리 검사 → 이 문서에 기록 → 통과하면 커밋.

## P2-01 — id 조회 최소 경로

**역할:** SQL의 `WHERE id = ?`가 B+ 트리를 거쳐 파일의 레코드 한 줄을 돌려주는 전체 경로를 연결한다. 소량 데이터로 확인한다.

- `src/bptree.{h,c}`: 메모리 B+ 트리. 차수 인자, 삽입(분할 연쇄·새 루트), 탐색, 중복 거부, 메모리 부족 시 무변경, dump·trace·validate.
- `src/storage.c`: `Table.index`. `table_open`에서 줄마다 (id, 위치) 등록, `table_append`에서 등록, `table_find_by_id` 신설.
- `src/executor.c`: `WHERE_ID`면 `table_find_by_id`, 아니면 기존 선형 탐색.
- `tools/bpt_demo.c`: 트리 모양과 탐색 경로를 보는 학습 도구.
- Phase 1 테스트 하나 변경: `test_executor_where_scans_whole_file` → `test_executor_id_uses_index_age_scans`(DB-P2-D07).

**직접 확인:** `examples/phase2_id_lookup.sql` 실행 → `build/bpt_demo 3 7 4`로 트리와 탐색 경로 관찰.

**P2-01 검증 기록(2026-11-15)**

| 명령 | 환경 | 결과 |
|---|---|---|
| `make` | macOS, Apple clang 17 | 경고 0 |
| `make test` | macOS | 단위 22,717개 검사 실패 0, CLI 32개 실패 0 |
| `leaks --atExit -- build/test_unit ...`, `... build/bpt_demo ...` | macOS | 누수 0 |
| `docker run --rm -v "$PWD:/src" -w /src gcc:13 make sanitize` | Linux, GCC 13, ASan/UBSan/LSan | 경고 0, 단위 22,717개·CLI 32개 실패 0, bpt_demo 정상, 검사 도구 보고 없음 |

단위 검사 수가 크게 늘어난 것은 B+ 트리 테스트가 키마다 CHECK를 부르기 때문이다(순차 2,000개 × 차수 5종, 무작위 3,000개 × 차수 4종 등).

spec 대응:

| ID | 테스트 |
|---|---|
| B1 | `test_bpt_split_shapes`, `test_bpt_sequential_many_orders`(앞 200개는 삽입마다 불변식 검사), `test_bpt_random_order_and_duplicates`(INT64_MIN/MAX·0·음수), `test_bpt_height_is_logarithmic`, `test_bpt_create_order_bounds`, `test_bpt_empty` |
| B2 | `test_bpt_nomem_leaves_tree_unchanged`: 연쇄 분할에 필요한 할당 9번 중 어느 번째에서 실패해도 트리 무변경, 이후 재삽입 성공 |
| B3 | 단위 `test_storage_index_lookup`·`_rebuilt_on_open`·`_many_records`(5,000건, 한글 섞인 가변 길이, 재시작 후 재확인), CLI `p2_id_lookup_after_restart`·`_same_run_after_insert`·`_with_gaps_in_file`·`p2_index_result_matches_full_scan` |
| B4 | `test_executor_id_uses_index_age_scans`: 열린 뒤 2번째 줄을 망가뜨리면 `id = 1`은 성공, `id = 2`는 "인덱스 불일치", `age = 25`는 "2번째 줄 손상" |

남은 제한: 기록 성공 후 인덱스 등록만 메모리 부족으로 실패하는 경로는 트리 단위 테스트로만 확인했다(Storage에서 실패 주입은 하지 않음). 이때 동작은 design.md "파일 위치와 일관성"에 적었다.

## P2-02 — EXPLAIN 실행 계획

**역할:** 조회 경로 판단을 `executor_plan` 한 곳으로 모으고, 그 결과(`QueryPlan`)를 실행 없이 보여 준다. 계약은 spec "EXPLAIN 계약", 설계는 design "실행 계획"(DB-P2-D09).

- `src/parser.{h,c}`: `Statement.explain`. `EXPLAIN SELECT ...`만 허용, `EXPLAIN INSERT`·`EXPLAIN` 뒤 다른 토큰은 오류.
- `src/storage.{h,c}`: `table_index_height`, `table_row_count`(메모리 인덱스에서 읽음, 파일 접근 없음).
- `src/executor.{h,c}`: `PlanKind`, `PlanStats`(계획 시점 통계), `QueryPlan`, `executor_plan`. `executor_run`의 SELECT가 계획으로 경로를 고르고, `explain`이면 `on_plan`만 호출. `ResultSink.on_plan` 추가(구조체 끝).
- `src/main.c`: `print_plan`이 한 줄 출력, EXPLAIN에는 헤더 없음.
- 기존 테스트 변경: 위치 초기화 `Statement`·`ResultSink`에 새 필드 값 추가(동작 변경 없음).

**직접 확인:** `build/minidb --data-dir data/p2e examples/phase2_explain.sql`

**P2-02 검증 기록(2026-11-16)**

| 명령 | 환경 | 결과 |
|---|---|---|
| `mingw32-make` | Windows, TDM-GCC | 새 경고 0 (storage.c `PRId64` 경고 5줄은 P2-01부터 있던 Windows 전용) |
| `mingw32-make test` | Windows | 단위 22,855개 검사 실패 0, CLI 35개 실패 0 |
| `docker run --rm -v "$PWD:/src" -w /src gcc:13 bash -c "make clean; make sanitize"` | Linux, GCC 13, ASan/UBSan/LSan | 경고 0, 단위 22,855개·CLI 35개 실패 0, bpt_demo 정상, 검사 도구 보고 없음 |
| 변이 확인: EXPLAIN 뒤 `return 0` 제거(계획 후 실행까지 진행) | Windows | 단위 4곳 실패 → 테스트가 "실행하지 않음"을 잡는다. 확인 후 원복 |

macOS(Apple clang) 빌드·`leaks`는 이번 단위에서 실행하지 않았다(작업 환경 Windows).

spec 대응:

| ID | 테스트 |
|---|---|
| B5 | 단위 `test_parse_explain`·`_errors`, `test_executor_plan_paths_and_stats`(빈 테이블 높이 1·행 0, 키 63→64개에서 높이 1→2, 재시작 후 같은 통계, 없는 id·음수도 INDEX LOOKUP, INSERT 계획 거부), `test_executor_explain_does_not_execute`(열린 뒤 2번째 줄을 망가뜨려도 EXPLAIN은 성공·on_row 호출 없음, 같은 조건 실제 SELECT는 손상 보고, on_plan 실패 전파). CLI `p2_explain_paths_and_stats`, `p2_explain_does_not_execute`(헤더 없음, 파일 불변, SELECT와 섞임), `p2_explain_errors`(fail-fast, 앞선 출력·INSERT 유지) |

## P2-03 — id 범위 조회

**역할:** `WHERE id` 범위 조건을 B+ 트리의 리프 연결로 처리한다. 계약은 spec "id 범위 조회 계약", 설계는 design "id 범위 조회"(DB-P2-D11).

- `src/parser.{h,c}`: `WHERE_ID_RANGE`, `CmpOp`(`= < <= > >= BETWEEN`), `Statement.where_op`·`where_value2`. age는 계속 `=`만.
- `src/bptree.{h,c}`: `bpt_range(lo, hi, 콜백)` — 시작 리프만 루트에서 찾고 next 연결을 따라감.
- `src/storage.{h,c}`: `table_find_id_range` — 첫 줄만 fseek, 이후 이어 읽기, 줄마다 id 재확인.
- `src/executor.{h,c}`: `PLAN_INDEX_RANGE_SCAN`, `IdRange`(양 끝 포함 정규화, 64비트 끝 넘침 방지), `QueryPlan`에 `where_op`·`where_value2`·`range`. `row_matches`도 범위 조건 처리.
- `src/main.c`: `INDEX RANGE SCAN (조건, B+ 트리 높이 h)`.
- `src/common.h`: MinGW에서 `db_error_set` 형식 검사를 `gnu_printf`로. P2-01부터 있던 Windows `PRId64` 경고 5줄이 사라짐(동작 변경 없음).
- 기존 테스트 변경: 위치 초기화 `Statement`에 새 필드 값, P2-02 테스트 중 `id > 1`·`id >= 1`을 오류로 기대하던 2곳을 `id <> 1`로(이제 유효한 문법이므로).

**직접 확인:** `build/minidb --data-dir data/p2r examples/phase2_range.sql`

**P2-03 검증 기록(2026-11-16)**

| 명령 | 환경 | 결과 |
|---|---|---|
| `mingw32-make` | Windows, TDM-GCC | 경고 0 |
| `mingw32-make test` | Windows | 단위 23,202개 검사 실패 0, CLI 40개 실패 0 |
| `docker run --rm -v "$PWD:/src" -w /src gcc:13 bash -c "make clean; make sanitize"` | Linux, GCC 13, ASan/UBSan/LSan | 경고 0, 단위 23,202개·CLI 40개 실패 0, bpt_demo 정상, 검사 도구 보고 없음 |
| 변이 확인 4종 | Windows | `bpt_range` 시작 위치 무시(59곳 실패), 범위 조회 id 재확인 제거(2곳), `id < v` 경계 포함(3곳), 뒤집힌 BETWEEN 무시(1곳) 모두 잡힘. 처음에 id 재확인 제거를 놓쳐 "형식은 정상이고 id만 다른 줄" 테스트를 추가함. 확인 후 원복 |

spec 대응:

| ID | 테스트 |
|---|---|
| B6 | 단위 `test_bpt_range`(차수 3·4·5·64 × 키 0~1000개, 없는 키에서 시작·끝, 리프 경계, 뒤집힘, INT64 끝, 콜백 멈춤 전파), `test_parse_id_range`·`_errors`, `test_executor_id_range`(빈 번호 파일, 모든 연산자 경계, INT64 끝 넘침 없음, 계획의 정규화 범위), `test_executor_range_matches_scan`(300건, 리프 경계를 넘는 범위 다수), `test_executor_range_reads_only_inside`(범위 밖 손상 줄은 읽지 않음, id만 다른 줄은 불일치 오류). CLI `p2_range_matches_full_scan`(전체 조회에서 골라낸 결과와 바이트 비교 17종), `p2_range_same_run_and_restart`, `p2_range_with_gaps_in_file`, `p2_range_explain`, `p2_range_errors`(fail-fast) |

## P2-04 — 100만 건과 성능 비교

**역할:** 100만 건을 Phase 1 처리기로 넣고, 시작 비용과 조회 시간을 분리해 인덱스 조회와 선형 탐색을 비교한다. 계약은 spec "측정 계약", 결정은 DB-P2-D12, 결과는 [benchmark.md](benchmark.md).

- `src/main.c`: `--timing`(stderr에만), `ExecStats`, 단조 시계 `now_ms`(Windows QPC / POSIX `CLOCK_MONOTONIC`). `print_row`가 결과 행 수를 셈.
- `src/bptree.{h,c}`: `BptMemStats`, `bpt_mem_stats`(노드 수·요청 바이트).
- `src/storage.{h,c}`: `IndexStats`, `table_index_stats`.
- `tools/gen_sql.py`, `tools/bench.py`, Makefile `bench`(`BENCH_ROWS`로 건수 조절, test와 분리).

**결과 요약(Windows, 100만 건):** id 단건 0.01~0.04 ms, age 선형 탐색 700~840 ms(중앙값), 시작 917.5 ms, 인덱스 높이 4·32.7MB. 프로세스 전체로는 시작 비용 때문에 차이가 2배 미만.

**P2-04 검증 기록(2026-11-16)**

| 명령 | 환경 | 결과 |
|---|---|---|
| `mingw32-make` | Windows, TDM-GCC | 경고 0 |
| `mingw32-make test` | Windows | 단위 23,276개 검사 실패 0, CLI 45개 실패 0 |
| `python tools/bench.py --rows 1000000 --repeat 7` | Windows | 정상 종료 63.6 s, 결과를 benchmark.md에 기록 |
| `docker run --rm -v "$PWD:/src" -w /src gcc:13 bash -c "make clean; make sanitize"` | Linux, GCC 13, ASan/UBSan/LSan | 경고 0, 단위 23,276개·CLI 45개 실패 0(bench 스모크 포함), bpt_demo 정상, 검사 도구 보고 없음. 첫 실행에서 `storage.h`의 `<stddef.h>` 누락(Linux에서만 드러남)을 잡아 고침 |

spec 대응:

| ID | 테스트·기록 |
|---|---|
| B7 | 단위 `test_bpt_mem_stats`(빈 트리·분할 후 노드 수, 바이트가 노드 수에 비례), `test_storage_index_stats`(64건에서 높이 2·리프 2·내부 1). CLI `p2_timing_keeps_stdout`(stdout 불변, 측정 줄 형식·행 수), `p2_timing_insert_and_failure`(추가 1행, 실패 문장은 측정 없음), `p2_timing_usage`, `p2_gen_sql_feeds_cli`(LF, 값 규칙, 처리기로 삽입), `p2_bench_smoke`(300건으로 전체 흐름·표 내용). 100만 건 결과는 benchmark.md |

## P2-05 — 전체 검증·마무리

**역할:** Phase 2 전 기능을 함께 돌려 Phase 1 계약(A1~A8)과 Phase 2 계약(B1~B7)이 동시에 지켜지는지 확인하고 문서를 정리한다. 사용자 동작·인터페이스 변경 없음.

- CLI `p2_random_model` 추가: 시드 고정 무작위 문장 600개(INSERT 35%, `id =`·`BETWEEN`·`< <= > >=`·`age =`·조건 없음), 실행 3번(재시작 2번)을 Python 모델과 바이트 비교. 변이(`id <= v`를 `v-1`로)를 넣으면 실패함을 확인.
- `.gitignore`에 `__pycache__/`(bench.py가 gen_sql.py를 import하며 생김).
- 예제 전부 실행: `phase2_id_lookup`·`phase2_explain`·`phase2_range`·`third` 정상. `basic.sql`은 2번째 문장의 `nickname` 컬럼으로 일부러 실패하는 Phase 1 예제(fail-fast 확인용, 내용 변경 없음).

**P2-05 검증 기록(2026-11-16)**

| 명령 | 환경 | 결과 |
|---|---|---|
| `mingw32-make` | Windows, TDM-GCC 10.3 | 경고 0 |
| `mingw32-make test` | Windows | 단위 23,276개 검사 실패 0, CLI 46개 실패 0 |
| `docker run --rm -v "$PWD:/src" -w /src gcc:13 bash -c "make clean; make sanitize"` | Linux, GCC 13, ASan/UBSan/LSan | 경고 0, 단위 23,276개·CLI 46개 실패 0, bpt_demo 정상, 검사 도구 보고 없음 |
| macOS(Apple clang) 빌드·`leaks` | — | P2-02 이후 실행하지 않음(작업 환경 Windows). P2-01까지는 macOS에서 확인 |

spec 대응:

| ID | 확인 |
|---|---|
| B8 | Phase 1 CLI 테스트 28개(`a1_`~`a7_`, 주석·사용법 등)와 Phase 1 단위 테스트가 Phase 2 전 기능과 함께 통과(A1~A7). 단위·CLI 전체를 ASan/UBSan/LSan으로 다시 실행해 보고 없음(A8). Phase 1 계약 변경은 DB-P2-D07(`WHERE id`가 파일 끝까지 읽지 않음) 하나뿐 |

### Phase 2 전체 요약

| 기능 | 커밋 | 핵심 |
|---|---|---|
| P2-01 id 조회 | f331be6 | B+ 트리(차수 64), 시작 시 재구성, `WHERE id = ?` → 위치 → 한 줄 |
| P2-02 EXPLAIN | 668ca65 | `executor_plan` 한 곳에서 경로 결정, `QueryPlan`/`PlanStats`(계획 시점 통계) |
| P2-03 범위 조회 | 46256d9 | `BETWEEN`·`< <= > >=`, `bpt_range`(리프 연결), 첫 줄만 fseek |
| P2-04 측정 | e6616a7 | `--timing`(`ExecStats`, stderr), gen_sql·bench, 100만 건 id 0.02 ms vs age 0.7~0.84 s |
| P2-05 마무리 | (이 커밋) | 무작위 모델 회귀, 전체 메모리 검사, 문서 정리 |

## 공통 규칙 (Phase 1에서 이어짐)

- CLI 테스트는 실제 프로세스의 stdout·stderr·종료 코드·저장 파일을 확인한다.
- 경고를 켜고 빌드하고, Docker `gcc:13`에서 ASan/UBSan/LSan을 실행해 실제 명령과 결과를 기록한다.
- 커밋·푸시는 사용자 지시 후 작업 브랜치에만 한다. main 직접 커밋·푸시 금지.
