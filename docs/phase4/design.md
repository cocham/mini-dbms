# Phase 4 설계와 결정 — UPDATE·DELETE

## 결정 목록

| ID | 항목 | 상태 |
|---|---|---|
| DB-P4-D01 | 저장: 바뀐 내용 전체를 임시 파일에 쓰고 원래 파일과 바꿔치기(rename). 파일 형식·id 증가 순서 유지 | 확정(2026-11-18, 사용자) |
| DB-P4-D02 | 조회 결과는 항상 id 순서. 파일이 id 순서로 유지되므로 조회 경로는 바꾸지 않는다 | 확정(2026-11-18, 사용자) |
| DB-P4-D03 | WHERE 없는 UPDATE·DELETE는 구문 오류 | 확정(2026-11-18, 사용자) |
| DB-P4-D04 | 다음 id를 별도 파일 `users.next`에 보존(아래) | 확정(2026-11-18, 사용자) |
| DB-P4-D05 | 실패 처리: 바꿔치기 전 실패는 무변경, 바꿔치기 뒤에는 실패할 단계가 없게 순서를 짠다(POSIX). Windows는 예외 1개(아래) | 확정(2026-11-18, 사용자) |
| DB-P4-D06 | 결과: 바뀐 행 수. CLI `UPDATE n`·`DELETE n`, 서버 `{"type":"update","count":n}`. 0행이면 다시 쓰지 않음 | 확정(2026-11-18, 사용자) |
| DB-P4-D07 | 문법: `UPDATE users SET name = .., age = .. WHERE ..;`, `DELETE FROM users WHERE ..;`. `id` 수정·같은 컬럼 두 번·`EXPLAIN UPDATE` 거부 | 확정(2026-11-18, 사용자) |
| DB-P4-D08 | 잠금: UPDATE·DELETE는 INSERT처럼 배타 잠금. 다시 쓰는 동안 모든 조회가 기다린다 | 확정(2026-11-18, 사용자) |

## 확인이 필요한 결정 (요약)

| 항목 | 추천 | 이유 | 다른 선택 |
|---|---|---|---|
| D04 다음 id 보존 | `users.next` 파일에 마지막으로 부여한 id를 적는다. 가장 큰 id의 행을 지우는 DELETE에서만 갱신한다(임시 파일 + 바꿔치기). 시작할 때 `max(파일의 가장 큰 id, users.next)`를 쓴다. 파일이 없으면 지금처럼 파일의 가장 큰 id | 다시 쓰기 방식에서는 가장 큰 id 행이 파일에서 사라지므로, 파일만 보면 그 id를 다시 쓰게 된다. 지운 id를 다시 쓰면 "id 3을 지웠다"는 기록이 새 행을 가리키게 된다. INSERT마다 쓰지 않으므로 INSERT 비용은 그대로다 | 다시 써도 됨으로 정한다(파일 하나로 단순하지만 지운 id가 재사용됨) |
| D05 실패 처리 | 아래 "다시 쓰기 순서" | 바꿔치기(rename)는 한 번에 일어나므로, 그 앞에서 실패하면 원래 상태 그대로다. 그 뒤에 실패할 일을 없애면 문장 하나가 원자적이다 | 바꿔치기 뒤 실패를 중단 상태로 처리 |
| D06 행 수 | 조건에 맞은 행 수(값이 같아도 센다) | 구현이 단순하고 PostgreSQL과 같다. (정정 2026-11-18: 처음에 "다시 실행해도 같은 수가 나온다"고 적었으나 틀렸다. DELETE 재실행은 보통 0이고, 조건 컬럼을 바꾸는 UPDATE도 0이 된다. 재시도 판단은 spec "실패와 재시도") | 실제로 값이 바뀐 행만(MySQL 방식) |
| D07 문법 범위 | 위와 같음 | 기존 WHERE 형태를 그대로 쓴다. 조건 종류를 늘리는 것은 이번 범위가 아니다 | `SET age = age + 1` 같은 식 허용 |
| D08 잠금 | 배타 잠금 하나 | 다시 쓰는 동안 파일과 인덱스를 통째로 바꾸므로 읽기와 겹칠 수 없다. 잠금 규칙(잠금 하나, 그 안에서 다른 잠금 없음)을 그대로 지킨다 | 새 파일을 다 쓴 뒤 바꿔치기 순간에만 배타 잠금(읽기 잠금으로 쓰기) — 더 빠르지만 그동안 들어온 INSERT를 새 파일에 다시 반영해야 해서 복잡하다 |

## 다시 쓰기 순서 (D01, D05)

```text
[배타 잠금 안]
1. 대상 찾기:   WHERE를 SELECT와 같은 경로(INDEX LOOKUP / RANGE SCAN / FULL SCAN)로 평가해
                바뀔 행이 있는지 센다. 0이면 여기서 끝(파일을 건드리지 않음).
2. 새 파일 쓰기: 원래 파일을 처음부터 읽으며 users.tsv.tmp에 쓴다.
                 지울 행은 건너뛰고, 고칠 행은 새 값으로 쓰고, 나머지는 그대로 쓴다.
                 쓰면서 새 B+ 트리(id → 새 파일의 바이트 위치)를 함께 만든다.
3. 마무리:      tmp를 fflush·닫는다. (실패 → tmp 지우고 새 트리 버리고 STORAGE 오류. 원래 그대로)
4. 다음 id:     가장 큰 id를 지우는 경우에만 users.next.tmp에 쓰고 users.next로 바꿔치기.
                (실패 → 위와 같음. users.next는 "더 큰 값"만 의미가 있어서 먼저 써도 해가 없다)
5. 새 FILE* 열기: tmp를 "a+b"로 연다(INSERT용). (실패 → 위와 같음)
6. 바꿔치기:    rename(users.tsv.tmp, users.tsv). (실패 → 새 FILE* 닫고 위와 같음)
7. 교체:        Table의 fp·fd·인덱스·file_size를 새 것으로 바꾸고 옛 것을 닫고 해제한다.
                POSIX에서는 5에서 연 FILE*가 바꿔치기 뒤에도 같은 파일을 가리키므로 여기서 실패할 일이 없다.
```

- 2에서 원래 파일은 위치를 지정해 읽는다(P3-04의 `read_at`). 원래 파일은 바꿔치기 전까지 손대지 않는다.
- 새 트리를 만드는 동안 메모리가 두 배(트리 두 개)가 된다. 100만 행이면 트리 하나가 약 33 MB(Phase 2 측정)다. 새 트리를 못 만들면 3과 같은 실패로 처리한다.
- 남은 `users.tsv.tmp`(이전 실행이 중간에 죽은 경우)는 시작할 때 지운다. `users.tsv`는 바꿔치기 전까지 온전하므로 지워도 된다.

**Windows (CLI만):** Windows의 rename은 이미 있는 파일을 덮어쓰지 않고, 열려 있는 파일은 바꿀 수 없다. 그래서 Windows에서는 옛 FILE*와 새 FILE*를 닫고 `MoveFileExA(..., MOVEFILE_REPLACE_EXISTING)`로 바꾼 뒤 다시 연다. 이 "다시 열기"는 바꿔치기 뒤라 실패하면 되돌릴 수 없다. 그 경우는 중단 상태(`DB_HALTED_INDEX` 사유와 같은 "다시 실행하면 파일에서 복원")로 두고 CLI는 오류로 끝난다. 파일 자체는 온전하다. 서버는 POSIX 전용이라 이 경우가 없다.

## 실행 흐름

```text
CLI·서버 → parser_next(UPDATE/DELETE 문장) → db_exec(배타 잠금) → executor_run
  → executor: 값 검증(INSERT와 같은 규칙) → table_rewrite(t, 대상 조건, 바꿀 값) → 행 수를 sink.on_change로
```

| 파일 | 바뀌는 것 |
|---|---|
| `parser.{h,c}` | `STMT_UPDATE`, `STMT_DELETE`. `Statement`에 `set_name`(있으면 malloc 문자열), `set_age`, `has_set_age`. WHERE 필수 검사 |
| `executor.{h,c}` | UPDATE·DELETE 실행. 대상 조건은 SELECT의 조건 평가를 그대로 쓴다. `ResultSink`에 `on_change(kind, count, ctx)` 추가(기존 호출자는 NULL) |
| `storage.{h,c}` | `table_rewrite`(위 순서), `users.next` 읽기·쓰기, 시작 시 `users.tsv.tmp` 정리. 테스트 전용 실패 주입(임시 파일 쓰기·바꿔치기) |
| `db.c` | UPDATE·DELETE를 배타 잠금으로 실행. 실패해도 원래 상태가 남으므로 중단 상태로 바꾸지 않는다(Windows 다시 열기 실패만 예외) |
| `main.c` | `UPDATE n`·`DELETE n` 출력 |
| `server.c` | JSON `{"type":"update","count":n}`. 결과 자리(INSERT처럼 고정 크기)를 실행 전에 확보 |

조회 경로(`table_scan`·`table_find_by_id`·`table_find_id_range`)는 바꾸지 않는다. 파일이 계속 id 순서이고 바꿔치기는 배타 잠금 안에서만 일어나기 때문이다.

## 동시성

- UPDATE·DELETE는 배타 잠금 안에서 파일·인덱스를 통째로 바꾼다. 공유 잠금으로 읽는 SELECT는 바꿔치기 전이나 후의 상태 하나만 본다(C2 그대로).
- 쓰기 우선(glibc)이므로 UPDATE·DELETE가 기다리기 시작하면 새 SELECT는 그 뒤에 선다. 다시 쓰기가 길면 그동안 모든 요청이 기다린다(파일 크기에 비례, 이번 Phase에서는 재지 않음).
- 동시 정확성 테스트를 늘린다: 스레드들이 INSERT·UPDATE·DELETE·SELECT를 섞고, 끝나면 각 스레드가 기억한 "살아 있어야 할 행"과 실제 결과, 트리 불변식, 재시작 뒤 같은 내용을 확인한다.

## 테스트

| 종류 | 내용 |
|---|---|
| 단위(Parser) | 정상 형태, 오류(WHERE 없음, `SET id`, 같은 컬럼 두 번, SET 없음, EXPLAIN UPDATE), 문자열·주석 |
| 단위(Storage·Executor) | id·age·범위 조건, 0행(파일 그대로: 크기·수정 시각), 여러 행, 값 검증 실패 시 무변경, 다시 쓰기 뒤 인덱스 조회·범위 조회·트리 불변식, 다음 id(가장 큰 id 삭제·재시작), 실패 주입(임시 파일 쓰기·바꿔치기) 뒤 원래 상태와 tmp 정리 |
| CLI | 위 시나리오를 실제 프로세스로, 출력 `UPDATE n`·`DELETE n`, 재시작 |
| 서버 | JSON 결과, 오류 코드, 응답 예산, 동시 클라이언트로 섞기 |
| 동시 정확성·TSan | 위 "동시성" |
