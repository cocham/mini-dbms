# Phase 2 설계와 결정

## 결정 목록

| ID | 항목 | 상태 |
|---|---|---|
| DB-P2-D01 | 리프 값은 레코드 줄이 시작하는 **파일 바이트 위치**(id → offset). 행 전체를 메모리에 복사하지 않는다 | 확정(2026-11-15, 사용자) |
| DB-P2-D02 | 인덱스는 메모리에만 둔다. `table_open`이 파일 전체를 검증하면서 다시 만든다 | 확정(2026-11-15, 과제 요구 "메모리 기반"에서 도출, 사용자 확인) |
| DB-P2-D03 | 필수 범위 + id 범위 조회 + EXPLAIN | 확정(2026-11-15, 사용자) |
| DB-P2-D04 | 기능 단위로 구현하고, 기능마다 사용자가 실행·예측·수정 후 진행 요청 → 다음 기능 | 확정(2026-11-15, 사용자) |
| DB-P2-D05 | 문서 위치 docs/phase2/. Phase 1 문서는 그대로 둔다 | 확정(2026-11-15, 사용자) |
| DB-P2-D06 | 기본 차수 64(노드당 키 최대 63개). 트리 모듈은 차수를 인자로 받아 테스트·데모에서 3, 4 같은 작은 차수로 분할을 관찰한다 | 제안대로 적용(2026-11-15). P2-04 측정 후 재검토 가능 |
| DB-P2-D07 | Phase 1 계약 변경: `WHERE id`는 더 이상 파일 끝까지 읽지 않는다. 손상 검출은 시작 시 전체 검증 + 읽은 줄의 id 재확인으로 대신한다 | 적용(2026-11-15, 과제 요구 "ID 검색 시 인덱스 활용"의 직접 결과) |
| DB-P2-D09 | EXPLAIN은 계획만 만들고 실행하지 않는다. 계획은 `QueryPlan` 구조체(경로 + 조건 + 계획 시점 통계 `PlanStats`)로 Executor가 만들고, 출력 문구는 CLI가 정한다. `executor_run`도 같은 `executor_plan`으로 경로를 고른다. 출력: `INDEX LOOKUP (id = 3, B+ 트리 높이 2)`, `FULL SCAN (age = 25, 전체 행 수 5)` | 확정(2026-11-16, 사용자. "전체 행 수" 문구, 계획 시점 통계와 실행 측정값 구분 지시) |
| DB-P2-D10 | D04 변경: P2-02~P2-05는 단위 구현 → 테스트·메모리 검사로 검증 → 통과하면 바로 커밋·다음 단위. 인터페이스 변경 결정은 구현 전 제안. 학습은 별도 노트로, 진행 조건이 아니다 | 확정(2026-11-16, 사용자. 처음엔 Codex 검증 대기였으나 같은 날 자동 커밋으로 변경) |
| DB-P2-D11 | id 범위 조회: `BETWEEN`(양 끝 포함)과 `< <= > >=`, id에만. 뒤집힌 범위는 결과 없음. 내부에서는 양 끝 포함 `IdRange[lo, hi]`로 정규화하고 `bpt_range`(시작 리프 + next 연결) → `table_find_id_range`(첫 줄만 fseek, 이후 이어 읽기). EXPLAIN은 `INDEX RANGE SCAN (원래 조건, B+ 트리 높이)`, 건수 없음 | 확정(2026-11-16, 사용자) |
| DB-P2-D08 | 주 개발 환경을 macOS(Apple clang, make)로 옮긴다. Windows 빌드 경로는 Makefile에 유지. 메모리 검사는 Docker `gcc:13`에서 `make sanitize` | 적용(2026-11-15). macOS의 Apple clang ASan은 이 OS 버전에서 초기화 중 멈춰 사용하지 않는다 |

## 모듈 구성

```text
SQL 파일 → CLI → Parser → Statement → Executor ─┬→ Storage.table_find_by_id ─→ B+ 트리 → fseek → users.tsv
                                                 ├→ Storage.table_scan ───────────────────────→ users.tsv
                                                 └→ Storage.table_append ─→ users.tsv, B+ 트리 등록
```

| 모듈 | Phase 2에서 맡는 일 |
|---|---|
| bptree (신규) | 정수 키 → 정수 값 B+ 트리. 삽입·분할·탐색, 학습용 출력(dump, trace), 불변식 검사. **SQL도 파일도 모른다** |
| Storage | `Table`이 트리를 소유. 시작 시 줄마다 (id, 줄 시작 위치)를 트리에 넣고, INSERT 후 등록하고, `table_find_by_id`로 위치 이동 후 한 줄 읽기 |
| Executor | 조회 경로 선택: `WHERE_ID` → `table_find_by_id`, 그 밖 → `table_scan` + 조건 필터 |
| Parser · CLI | P2-01에서는 변경 없음. P2-02: Parser가 `EXPLAIN SELECT`를 `Statement.explain`으로, CLI가 `QueryPlan`을 한 줄로 출력. 범위 조회(P2-03)·옵션(P2-04)에서 더 바뀐다 |

트리를 Storage 안에 둔 이유: "id → 파일 위치"는 파일 형식과 함께 움직이는 정보다. Executor가 트리를 직접 만지면 파일 위치라는 Storage 내부 사정이 바깥으로 새어 나온다. 트리 모듈 자체는 값이 무엇을 뜻하는지 모르므로 Phase 3나 다른 인덱스에도 그대로 쓸 수 있다.

## B+ 트리 구조

```text
차수 m = 3 (자식 최대 3, 키 최대 2), 키 1~7을 차례로 넣은 결과 — build/bpt_demo 3 7

깊이 0 (루트):            [5]
                        /     \
깊이 1 (내부):        [3]      [7]
                     /   \    /   \
깊이 2 (리프):   [1 2] → [3 4] → [5 6] → [7]
```

| 규칙 | 내용 |
|---|---|
| 데이터 위치 | 키·값 쌍은 **리프에만** 있다. 내부 노드의 키는 길 안내용 구분 키다 |
| 구분 키 | 내부 노드 `keys[i]`는 `children[i+1]` 아래의 모든 키 이하. 찾는 키 k는 "k 이하인 구분 키 개수"번째 자식으로 내려간다 |
| 리프 연결 | 리프는 오름차순으로 `next`가 이어져 있다. 범위 조회(P2-03)는 시작 리프만 트리로 찾고 나머지는 옆으로 걷는다 |
| 균형 | 모든 리프는 같은 깊이. 높이는 분할이 루트까지 올라갈 때만 1 늘어난다 |
| 리프 분할 | 키가 m개가 되면 앞쪽 ⌈m/2⌉개를 남기고 나머지를 새 리프로. 새 리프의 첫 키를 부모에 **복사** |
| 내부 분할 | 가운데 키를 부모로 **이동**(양쪽 어디에도 남지 않음) |
| 중복 | 거부(`BPT_DUPLICATE`). id는 유일하다 |
| 메모리 부족 | 분할에 필요한 노드 수를 먼저 세어 모두 확보한 뒤 수정한다. 확보에 실패하면 트리는 그대로 |

### 메모리 사용 (기본 차수 64)

노드는 배열을 m칸(분할 직전의 한 칸 초과 포함) 미리 잡는다. 리프 하나 ≈ 키 64×8 + 값 64×8 + 구조체 ≈ 1KB.
자동 ID는 항상 오른쪽 끝에 들어가므로, 반으로 나누는 분할 규칙에서는 왼쪽에 남는 리프가 절반(32개)만 찬 채로 굳는다.
100만 건이면 리프 약 31,000개, 약 33MB로 예상한다. 실측과 개선(순차 삽입 시 오른쪽 끝 분할 최적화)은 P2-04에서 다룬다.

## 파일 위치(offset)와 일관성

```text
users.tsv (바이너리 모드, 줄 끝은 LF 1바이트)
offset 0   1<TAB>민수<TAB>25<LF>        ← 1 + 1 + 6(한글 2자 × 3바이트) + 1 + 2 + 1 = 12바이트
offset 12  2<TAB>지영<TAB>30<LF>
offset 24  3<TAB>O'Brien<TAB>25<LF>
```

- 시작 시: `ftell`을 줄마다 부르지 않고 "줄 길이 + 1"을 더해 위치를 직접 센다.
- INSERT 시: `fseek(끝)` → `ftell`로 새 줄 위치를 구한 뒤 기록 → 성공하면 트리에 등록.
- 등록이 메모리 부족으로 실패하면 레코드는 파일에 남고 오류를 보고한다. 다음 실행이 파일에서 인덱스를 다시 만들므로 파일 기준으로는 일관된다.
- 조회 시: 위치로 이동해 읽은 줄의 id가 찾던 id와 다르면 엉뚱한 행을 돌려주지 않고 오류로 보고한다(열린 뒤 다른 경로로 파일이 바뀐 경우).
- `fseek`은 `long`을 받는다. Windows에서는 2GB를 넘는 위치에서 오류로 보고한다(100만 건 ≈ 30MB라 범위 안).

## 실행 계획 (P2-02, DB-P2-D09)

```text
Statement ──executor_plan──▶ QueryPlan { kind, where, where_value, stats{tree_height, total_rows} }
                                 │
             st->explain ? ──────┼──▶ sink->on_plan(plan)  → CLI print_plan이 한 줄 출력 (조회 실행 안 함)
                                 └──▶ plan.kind에 따라 table_find_by_id / table_scan 실행
```

```c
/* executor.h (요약) */
typedef enum { PLAN_FULL_SCAN, PLAN_INDEX_LOOKUP } PlanKind;
typedef struct { int tree_height; int64_t total_rows; } PlanStats;  /* 계획 시점 통계 */
typedef struct { PlanKind kind; WhereKind where; int64_t where_value; PlanStats stats; } QueryPlan;
int executor_plan(const Table *t, const Statement *st, QueryPlan *out, DbError *err);
/* ResultSink에 int (*on_plan)(const QueryPlan *plan, void *ctx) 추가 */

/* storage.h */
int table_index_height(const Table *t);   /* bpt_height */
int64_t table_row_count(const Table *t);  /* bpt_size: 모든 레코드가 인덱스에 등록되므로 키 수 = 행 수 */
```

| 결정 | 이유 |
|---|---|
| 경로 판단을 `executor_plan` 한 곳에 | EXPLAIN이 보여 주는 경로와 실제 실행 경로가 갈라질 수 없다. P2-03의 INDEX RANGE SCAN도 여기에 분기 하나로 추가 |
| 계획은 구조체, 문구는 CLI | Executor는 stdout을 모른다(Phase 1 원칙). Phase 3 서버는 같은 구조체를 JSON 등 다른 형식으로 낼 수 있다 |
| `PlanStats`라는 별도 타입 | 계획 시점 통계(트리 높이, 행 수)와 P2-04의 실행 측정값(시간 등)을 타입 수준에서 섞지 않는다 |
| 통계는 인덱스에서 | 시작 시 구축된 메모리 인덱스만 보므로 계획 생성이 파일을 추가로 읽지 않는다. 행 수를 따로 세는 필드를 두지 않아 어긋날 일이 없다 |
| 경로는 조건 종류로만 | 실제 DB의 비용 기반 선택(선택도 추정 등)은 범위 밖. id 조건이면 항상 인덱스가 이득이고 그 밖에는 인덱스가 없다 |

한계: 인덱스 등록만 메모리 부족으로 실패한 레코드(design "파일 위치와 일관성")는 파일에는 있지만 키 수에 빠져, 그 실행 동안 전체 행 수가 실제보다 작게 보일 수 있다. 이 경우 INSERT가 오류를 보고하므로 CLI는 fail-fast로 멈춘다.

## id 범위 조회 (P2-03, DB-P2-D11)

```text
WHERE id >= 5  ─parser─▶  WHERE_ID_RANGE, where_op = CMP_GE, where_value = 5
               ─executor_plan─▶  PLAN_INDEX_RANGE_SCAN, range = [5, INT64_MAX]
               ─executor_run─▶  table_find_id_range(5, INT64_MAX)
                                  └ bpt_range: 루트 → lo가 있을 리프 → next → next ... (키 > hi에서 멈춤)
                                      └ range_visit: 첫 키 위치로 fseek, 다음부터는 이어서 read_line + id 재확인
```

| 결정 | 이유 |
|---|---|
| 비교 형태(`CmpOp`)와 정규화 범위(`IdRange`)를 둘 다 보관 | 실행은 한 가지 형태 `[lo, hi]`만 다루면 되고, EXPLAIN은 사용자가 쓴 형태를 그대로 보여 준다 |
| 정규화에서 `v - 1`, `v + 1`이 넘칠 때는 빈 범위 | `id < INT64_MIN`, `id > INT64_MAX`가 오버플로로 전체 범위가 되는 것을 막는다 |
| 뒤집힌 BETWEEN은 결과 없음 | SQL 표준과 같다. 빈 범위는 트리도 파일도 보지 않는다 |
| 트리는 콜백 순회(`bpt_range`) | 트리 모듈은 여전히 파일을 모른다. 멈춤 신호로 오류를 바로 전파한다 |
| 파일은 첫 줄에서만 이동 | id가 증가 순서로 저장되고 모든 줄이 인덱스에 있으므로, 연속된 키는 파일에서도 연속된 줄이다. 줄마다 fseek하면 stdio 버퍼를 매번 버린다. 위치가 이어지지 않으면 그 위치로 다시 이동한다 |
| `row_matches`에도 범위 조건 | 선형 탐색으로 같은 조건을 처리할 때도 같은 결과가 나오게 한다(인덱스 경로와 비교 기준) |

## 시작 비용에 대한 주의

CLI는 실행할 때마다 `table_open`에서 파일 전체를 읽는다(Phase 1의 검증 + Phase 2의 인덱스 구축). 따라서 **프로세스 전체 실행 시간**에는 항상 O(N) 시작 비용이 들어간다.
인덱스의 효과는 "시작한 뒤 조회 한 번에 걸리는 시간"에서 드러나므로, P2-04의 성능 비교는 시작 비용과 조회 시간을 분리해서 잰다.
Phase 3의 API 서버는 한 번 시작해 계속 떠 있으므로 이 시작 비용은 한 번만 든다.
