# Phase 2 구현설명서

위에서 아래로 읽는 지도. 기능이 하나 끝날 때마다 절이 늘어난다. 지금은 **P2-04(100만 건과 측정)**까지 있다.

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

## 6. 이후 절

7절 EXPLAIN, 8절 범위 조회, 9절 100만 건과 측정이 아래에 이어진다.

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

## 8. 범위 조회 — 리프 연결 리스트 걷기 (P2-03)

```sql
SELECT * FROM users WHERE id BETWEEN 2 AND 4;
```

```text
Parser    WHERE_ID_RANGE, where_op = CMP_BETWEEN, where_value = 2, where_value2 = 4
Executor  executor_plan → PLAN_INDEX_RANGE_SCAN, range = [2, 4]   (id < v → [INT64_MIN, v-1] 등으로 정규화)
          executor_run  → range.empty면 끝, 아니면 table_find_id_range(2, 4)
Storage   bpt_range(2, 4, range_visit)
            ① 루트에서 2가 있을 리프까지 내려감 (bpt_search와 같은 길)
            ② 리프 안 lower_bound(2)부터 키를 하나씩: 키 > 4면 끝
            ③ 리프 끝이면 next로 옆 리프, pos = 0부터 계속
          range_visit(id, 위치)
            첫 키: fflush + fseek(위치) / 다음 키: 위치가 이어지면 그대로 read_line
            parse_record → id 재확인 → print_row
```

**왜 루트로 다시 올라가지 않나:** 리프는 오름차순으로 `next`가 이어져 있어서, 시작점만 찾으면 나머지는 옆으로 걷기만 하면 된다. 그래서 비용은 O(log N + K)이다. `bpt_demo 3 7`의 "리프 연결" 줄이 이 길이다.

**왜 파일에서도 한 번만 이동하나:** id는 증가 순서로만 저장되고(append 전용, 자동 증가) 모든 줄이 인덱스에 있으므로, 연속된 키는 파일에서도 바로 이웃한 줄이다. 줄마다 fseek하면 stdio 버퍼를 매번 버리게 된다.

**넘침 주의:** `id < v`를 `[.., v-1]`로 바꿀 때 v가 INT64_MIN이면 `v-1`이 넘친다. 이때는 빈 범위로 처리한다(`id_range_of`).

## 9. 100만 건과 측정 — 시작 비용 vs 조회 비용 (P2-04)

```text
tools/gen_sql.py 1000000 > insert.sql          i번째: ('user%06d', i % 151)
minidb --data-dir D insert.sql                  Phase 1 처리기 그대로: 100만 번 parser_next → executor_run(INSERT)
minidb --data-dir D --timing query.sql > out    측정 줄은 stderr

main
  t0 = now_ms(); table_open(...)                ← [측정] 시작: 파일 전체 검증 + 줄마다 bpt_insert
  print_timing_start → table_index_stats        ← [측정] 인덱스: bpt_mem_stats가 노드를 훑어 셈
  문장마다: es.rows_returned = 0; t0 = now_ms()
            executor_run(...)                   ← print_row가 es.rows_returned++
            es.elapsed_ms = now_ms() - t0       ← [측정] 문장 k: ... ms, 결과 n행
```

| 값 | 타입 | 언제 정해지나 | 어디서 보나 |
|---|---|---|---|
| 트리 높이, 전체 행 수 | `PlanStats` | 계획 시점(실행 전) | `EXPLAIN` stdout |
| 시작 시간, 문장 시간, 결과 행 수 | `ExecStats` | 실행 후 | `--timing` stderr |
| 노드 수, 인덱스 메모리 | `IndexStats` | 시작 직후 | `--timing` stderr |

**왜 시작과 문장을 나눠 재나:** CLI는 실행할 때마다 `table_open`이 파일 전체를 읽는다(O(N)). 프로세스 전체 시간으로 비교하면 id 조회도 이 비용에 묻혀 선형 탐색과 비슷해 보인다. 인덱스의 효과는 "시작한 뒤 조회 한 번"에서 드러난다. 측정값은 [benchmark.md](benchmark.md).

**`tools/bench.py`:** 생성·삽입 후 같은 조회 묶음(id 처음/가운데/끝/없음, id 범위 1,000건, age 일치 많음/없음)을 프로세스 7번 실행해 문장별 중앙값을 낸다. 계획 열은 같은 조건의 EXPLAIN 출력이라 측정값이 아니다.
