# Phase 2 구현 순서

## 현재 상태

P2-02 완료(2026-11-16). 다음은 P2-03.

진행 방식(2026-11-16 변경, DB-P2-D10): 한 단위 구현 → 단위·CLI 테스트와 메모리 검사로 검증 → 통과하면 바로 커밋하고 다음 단위. 사용자 동작이나 공개 인터페이스를 바꾸는 결정은 구현 전에 짧게 제안한다. 학습은 사용자의 별도 노트로 진행하며 진행 조건이 아니다. Phase 3는 시작하지 않는다.

| ID | 기능 | 완료 기준 | 상태 |
|---|---|---|---|
| P2-00 | 구현 전 결정 | design.md DB-P2-D01~D08 | 완료(2026-11-15) |
| P2-01 | id 조회 최소 경로 | B+ 트리 삽입·분할·탐색, 시작 시 인덱스 구축, INSERT 시 등록, `WHERE id = ?`가 트리 → 위치 → 한 줄. spec B1~B4 | 완료(커밋 f331be6) |
| P2-02 | EXPLAIN 실행 계획 | `EXPLAIN SELECT ...`가 INDEX LOOKUP / FULL SCAN과 계획 시점 통계(트리 높이, 전체 행 수)를 보여 줌. spec B5 | 완료 |
| P2-03 | id 범위 조회 | `BETWEEN`, `<` `<=` `>` `>=`를 리프 연결로 처리, EXPLAIN에 INDEX RANGE SCAN. spec B6 | 대기 |
| P2-04 | 100만 건과 성능 비교 | 대량 SQL 생성, 시작 비용과 조회 시간 분리 측정, 인덱스 vs 선형 탐색 결과 기록. spec B7 | 대기 |
| P2-05 | 전체 검증·마무리 | Phase 1 회귀, 메모리 검사, 문서 정리. spec B8 | 대기 |

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

## 공통 규칙 (Phase 1에서 이어짐)

- CLI 테스트는 실제 프로세스의 stdout·stderr·종료 코드·저장 파일을 확인한다.
- 경고를 켜고 빌드하고, Docker `gcc:13`에서 ASan/UBSan/LSan을 실행해 실제 명령과 결과를 기록한다.
- 커밋·푸시는 사용자 지시 후 작업 브랜치에만 한다. main 직접 커밋·푸시 금지.
