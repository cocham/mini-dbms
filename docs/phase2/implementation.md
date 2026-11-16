# Phase 2 구현설명서

위에서 아래로 읽는 지도. 기능이 하나 끝날 때마다 절이 늘어난다. 지금은 **P2-02(EXPLAIN)**까지 있다.

## 1. 무엇이 바뀌었나 — 한 장 요약

```text
Phase 1:  SELECT ... WHERE id = 3  →  1번째 줄, 2번째 줄, ..., N번째 줄 전부 읽고 비교
Phase 2:  SELECT ... WHERE id = 3  →  B+ 트리에서 3 → 24(바이트 위치)  →  fseek(24)  →  한 줄 읽기
```

| 역할 | 코드 | P2-01에서 확인할 것 |
|---|---|---|
| B+ 트리 | [bptree.h](../../src/bptree.h), [bptree.c](../../src/bptree.c) | `bpt_search`가 내려가는 방식, `bpt_insert`의 분할 |
| Storage | [storage.c](../../src/storage.c) | `scan_file`의 위치 세기, `table_append`의 등록, `table_find_by_id` |
| Executor | [executor.c](../../src/executor.c) | `WHERE_ID`일 때만 경로가 갈라지는 `executor_run` |
| 학습 도구 | [bpt_demo.c](../../tools/bpt_demo.c) | 트리 모양·탐색 경로 출력 |

## 2. 실행 한 번의 흐름

```text
table_open
  └ scan_file(t, build=t->index)    파일 전체 검증 + 줄마다 bpt_insert(id, 줄 시작 위치)
parser_next → executor_run 반복
  ├ INSERT        → table_append    파일 끝 위치 ftell → 한 줄 기록 → bpt_insert(id, 위치)
  ├ WHERE id = k  → table_find_by_id
  │                  bpt_search(k) → 위치 → fseek → read_line → parse_record → id 재확인 → 콜백
  └ 그 밖         → table_scan      Phase 1 그대로
table_close        → bpt_destroy (트리는 메모리와 함께 사라짐)
```

P2-01에서 Parser와 CLI는 한 줄도 바뀌지 않았다(P2-02에서 EXPLAIN 때문에 바뀜, 7절). "어떤 경로로 찾을지"는 Executor가, "어디에 있는지"는 Storage가 안다.

## 3. WHERE id = 3 — 트리를 거쳐 한 줄을 읽기까지

```sql
SELECT * FROM users WHERE id = 3;
```

```text
Executor  executor_run: st->where == WHERE_ID → table_find_by_id(t, 3, print_row)
    ↓
Storage   table_find_by_id
  ① bpt_search(index, 3, &offset)      없으면 여기서 0 반환: 파일을 읽지 않음, 헤더만 출력
  ② fseek(fp, offset=24)
  ③ read_line → "3<TAB>O'Brien<TAB>25"
  ④ parse_record → Row{id=3, ...}, row.id == 3 확인
  ⑤ print_row 콜백 한 번
```

**①의 내부 — bpt_search**

```c
while (!n->is_leaf) {
    n = n->children[child_index(n, key)];   /* 노드 안에서 이진 탐색 → 내려갈 자식 */
}
pos = lower_bound(n, key);                  /* 리프 안에서 이진 탐색 → 정확히 일치? */
```

루프는 트리 높이만큼만 돈다. 차수 64에서 100만 건이면 높이 4 정도라, 노드 4개를 거쳐 위치를 얻는다.
`build/bpt_demo 3 7 4`로 같은 과정을 글로 볼 수 있다.

**④를 하는 이유:** 인덱스는 "그 위치에 그 id가 있다"는 약속이다. 프로그램이 열려 있는 동안 다른 경로로 파일이 바뀌면 약속이 깨지는데, 이때 엉뚱한 행을 내보내는 대신 오류를 낸다. 이 검사를 이용해 "id 조회는 정말 한 줄만 읽는다"를 테스트로 증명한다(`test_executor_id_uses_index_age_scans`).

## 4. 인덱스는 언제 만들어지나

| 시점 | 함수 | 위치를 얻는 방법 |
|---|---|---|
| 프로그램 시작 | `table_open` → `scan_file(build)` | `offset += 줄 길이 + 1`로 직접 셈 |
| INSERT 한 건 | `table_append` | 기록 직전 `fseek(끝)` + `ftell` |

트리는 파일에서 언제든 다시 만들 수 있는 파생 데이터라 디스크에 저장하지 않는다(DB-P2-D02). 대신 매 실행 시작에 O(N) 비용이 든다. 이 비용은 P2-04에서 조회 시간과 분리해 잰다.

## 5. bpt_insert — 넘치면 나눈다

```text
1) 리프까지 내려가며 경로(path, slot)를 기억
2) 분할이 몇 번 연쇄될지 세고, 새 노드를 먼저 전부 확보   ← 실패하면 트리는 그대로
3) 리프에 끼워 넣기 (여기부터는 실패하지 않음)
4) 리프가 넘쳤으면 split_leaf → 부모에 (구분 키, 새 노드) 삽입 → 부모도 넘치면 split_internal ...
5) 루트까지 넘쳤으면 새 루트 → 높이 + 1
```

| 분할 | 위로 올리는 키 | 이유 |
|---|---|---|
| 리프 | 오른쪽 리프의 첫 키를 **복사** | 데이터(키·값)는 리프에 남아 있어야 한다 |
| 내부 | 가운데 키를 **이동** | 내부 키는 길 안내용이라 한 곳에만 있으면 된다 |

## 6. 다음 기능에서 이어질 절

- 8절 범위 조회 — 리프 연결 리스트 걷기 (P2-03)
- 9절 100만 건과 측정 — 시작 비용 vs 조회 비용 (P2-04)

## 7. EXPLAIN — 경로 선택을 바깥에서 보기 (P2-02)

```sql
EXPLAIN SELECT * FROM users WHERE id = 3;    -- INDEX LOOKUP (id = 3, B+ 트리 높이 1)
EXPLAIN SELECT * FROM users WHERE age = 25;  -- FULL SCAN (age = 25, 전체 행 수 3)
```

```text
Parser    "EXPLAIN" 다음 "SELECT"면 parse_select + st.explain = 1   (EXPLAIN INSERT는 오류)
    ↓
CLI       st.explain이면 "id name age" 헤더를 찍지 않음
    ↓
Executor  executor_run(SELECT)
  ① executor_plan(t, st, &plan)        경로 판단은 여기 한 곳: WHERE_ID → INDEX LOOKUP, 그 밖 → FULL SCAN
       plan.stats.tree_height = table_index_height(t)   ← bpt_height, 메모리만 봄
       plan.stats.total_rows  = table_row_count(t)      ← bpt_size,   메모리만 봄
  ② st.explain이면 sink->on_plan(&plan) 후 끝     조회 실행 없음, 파일 접근 없음
  ③ 아니면 plan.kind로 table_find_by_id / table_scan
    ↓
CLI       print_plan: QueryPlan → 한 줄 문구
```

| 타입 | 담는 것 | 담지 않는 것 |
|---|---|---|
| `QueryPlan` | 경로(`PlanKind`), 조건, `PlanStats` | 출력 문구(CLI 몫) |
| `PlanStats` | 계획 시점 통계: 트리 높이, 전체 행 수 | 실행 측정값(시간 등, P2-04에서 별도 타입) |

**"파일을 읽지 않는다"의 범위:** 시작할 때 `table_open`은 여전히 파일 전체를 읽어 인덱스를 만든다. EXPLAIN이 아끼는 것은 그 뒤의 읽기다. 계획 생성은 이미 메모리에 있는 트리에서 높이와 키 수만 꺼낸다.
`test_executor_explain_does_not_execute`가 이를 증명한다. 열린 뒤 2번째 줄을 망가뜨려도 `EXPLAIN ... WHERE age = 25`는 성공하고, 같은 조건의 실제 SELECT는 2번째 줄 손상을 보고한다.

**①을 `executor_run`도 쓰는 이유:** EXPLAIN이 "INDEX LOOKUP"이라고 말하는데 실제 실행은 선형 탐색을 타는 일이 구조적으로 불가능해진다. P2-03의 INDEX RANGE SCAN도 ①에 분기 하나를 더하면 EXPLAIN과 실행에 동시에 반영된다.
