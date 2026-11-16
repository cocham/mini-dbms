# Phase 2 구현 순서

## 현재 상태

P2-01 구현·테스트 완료, **사용자 실습 대기**(2026-11-15). 사용자가 [learning.md](learning.md)의 P2-01 실습을 마치고 진행을 요청하면 P2-02를 시작한다.

| ID | 기능 | 완료 기준 | 상태 |
|---|---|---|---|
| P2-00 | 구현 전 결정 | design.md DB-P2-D01~D08 | 완료(2026-11-15) |
| P2-01 | id 조회 최소 경로 | B+ 트리 삽입·분할·탐색, 시작 시 인덱스 구축, INSERT 시 등록, `WHERE id = ?`가 트리 → 위치 → 한 줄. spec B1~B4 | 테스트 PASS, 사용자 실습 대기 |
| P2-02 | EXPLAIN 실행 계획 | `EXPLAIN SELECT ...`가 INDEX LOOKUP / FULL SCAN과 트리 높이를 보여 줌. spec B5 | 대기 |
| P2-03 | id 범위 조회 | `BETWEEN`, `<` `<=` `>` `>=`를 리프 연결로 처리, EXPLAIN에 INDEX RANGE SCAN. spec B6 | 대기 |
| P2-04 | 100만 건과 성능 비교 | 대량 SQL 생성, 시작 비용과 조회 시간 분리 측정, 인덱스 vs 선형 탐색 결과 기록. spec B7 | 대기 |
| P2-05 | 전체 검증·마무리 | Phase 1 회귀, 메모리 검사, 문서 정리. spec B8 | 대기 |

기능마다 하는 일: 구현 → 단위·CLI 테스트 → 메모리 검사 → 이 문서에 기록 → learning.md에 실습 추가 → 멈춤.

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

## 공통 규칙 (Phase 1에서 이어짐)

- CLI 테스트는 실제 프로세스의 stdout·stderr·종료 코드·저장 파일을 확인한다.
- 경고를 켜고 빌드하고, Docker `gcc:13`에서 ASan/UBSan/LSan을 실행해 실제 명령과 결과를 기록한다.
- 커밋·푸시는 사용자 지시 후 작업 브랜치에만 한다. main 직접 커밋·푸시 금지.
