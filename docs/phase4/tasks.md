# Phase 4 구현 순서 — UPDATE·DELETE

## 현재 상태

**P4-01~P4-04 구현 완료(2026-11-18), 사용자 검증 대기.** 브랜치 `phase4-update-delete`(`phase3-server`의 `f8f88b4`에서 시작). D01~D08 확정, 100만 행 측정은 하지 않기로 했다(사용자).

진행 방식: P4-01까지는 단위마다 사용자 검증(PASS)을 받았다. P4-02부터는 사용자가 "단계를 합쳐서 다 만들어 놓고 검증해도 된다"고 해서 P4-02~P4-04를 이어서 구현·자체 검증·커밋하고 한 번에 검증받는다. PASS 뒤 push·main 대상 PR.

| ID | 기능 | 완료 기준 | 상태 |
|---|---|---|---|
| P4-00 | 구현 전 결정 | design.md DB-P4-D01~D08 확정 | 완료(2026-11-18) |
| P4-01 | 문법 | D1 | 완료 |
| P4-02 | 파일 다시 쓰기와 CLI | D2, D3, D4 | 완료(검증 대기) |
| P4-03 | 서버와 동시성 | D5 | 완료(검증 대기) |
| P4-04 | 문서와 실습 | D6 | 완료(검증 대기) |

## 단위별 흐름과 확인 시나리오

### P4-01 — 문법

**흐름:** Parser가 `UPDATE ... SET ... WHERE ...;`와 `DELETE FROM ... WHERE ...;`를 `Statement`로 만든다. 실행은 아직 하지 않는다. CLI·서버는 이 문장을 받으면 "아직 지원하지 않는 문장"(실행 오류, 아무것도 바뀌지 않음)으로 답한다.

**확인 시나리오:**
- `UPDATE users SET age = 31 WHERE id = 3;`는 파싱되고, CLI는 "아직 지원하지 않음" 오류로 끝난다.
- `DELETE FROM users;`(WHERE 없음), `UPDATE users SET id = 5 WHERE id = 1;`, `UPDATE users SET age = 1, age = 2 WHERE id = 1;`, `EXPLAIN DELETE FROM users WHERE id = 1;`은 구문 오류이고, 서버에서는 같은 요청의 앞 문장도 실행되지 않는다(전체 선파싱).

**파일:** `src/parser.{h,c}`, `src/executor.c`(미지원 오류), `tests/test_unit.c`, `tests/cli_test.py`.

**구현 메모 (2026-11-18):**
- `StmtKind`에 `STMT_UPDATE`·`STMT_DELETE`. `Statement`에 `set_cols`(`SET_COL_NAME | SET_COL_AGE`)를 더하고, SET 값은 INSERT와 같은 `name`·`age` 필드에 담는다. WHERE는 SELECT의 `parse_where`를 그대로 쓴다.
- 문장 시작이 맞지 않을 때의 메시지가 "INSERT 또는 SELECT" → "INSERT, SELECT, UPDATE 또는 DELETE"로 바뀌었다. `EXPLAIN UPDATE`·`EXPLAIN DELETE`는 "EXPLAIN은 SELECT 문에만"(전에는 "EXPLAIN 뒤에 SELECT"). 이 두 문구를 기대하던 기존 테스트 4곳을 고쳤다(의도한 변경).
- `db.c`: UPDATE·DELETE도 배타 잠금(DB-P4-D08). `server.c`: UPDATE·DELETE는 결과를 쓰지 않고 실행 오류로 끝난다(JSON 결과는 P4-03).
- 기존 테스트의 위치 순서 `Statement` 초기화 34곳에 새 필드(`set_cols = 0`)를 더했다(경고 0 유지).

**P4-01 검증 기록(2026-11-18)**

| 명령 | 환경 | 결과 |
|---|---|---|
| `mingw32-make`, `mingw32-make test` | Windows, TDM-GCC 10.3 | 경고 0, 단위 29,218개·CLI 48개 실패 0 |
| `make clean && make test` | Docker `gcc:13` | 경고 0, 단위 29,226개·CLI 48개·서버 24개·부하 생성기 9개 실패 0 |
| `make sanitize` | Docker `gcc:13`, ASan/UBSan/LSan | 실패 0, 보고 없음 |
| `make tsan` | Docker `gcc:13`, TSan | 단위·서버 실패 0, 보고 없음 |
| 변이 5개 | Docker `gcc:13` | 모두 잡힘: WHERE 없는 문장 허용, `SET id` 허용, SET 중복 검사 제거, `EXPLAIN UPDATE` 통과, UPDATE·DELETE 실행이 성공으로 끝남 |

spec 대응:

| ID | 테스트 |
|---|---|
| D1 | 단위 `test_parse_update_delete`(정상 6형태·대소문자·여러 문장, 오류 24가지: WHERE 없음, `SET id`, 중복, 없는 컬럼, SET·`=` 빠짐, 타입, 범위, name 조건, AND, 테이블, 세미콜론, FROM, age 비교, EXPLAIN), `test_executor_update_delete_not_yet`(실행 오류, 파일 무변경). CLI `p4_update_delete_parse_but_not_executed_yet`, `p4_update_delete_syntax_errors`. 서버 `update_delete_syntax_and_not_yet`(WHERE 없는 DELETE → 400 `SQL_SYNTAX` statement 2, 앞 INSERT도 미실행 / UPDATE → 400 `SQL_ERROR`, 앞 INSERT만 실행) |

### P4-02 — 파일 다시 쓰기와 CLI

**흐름:** Executor가 값을 검증하고 `table_rewrite`로 임시 파일에 새 내용을 쓴 뒤 바꿔치기한다. 새 인덱스로 교체한다. CLI는 `UPDATE n`·`DELETE n`을 출력한다. 다음 id는 `users.next`로 보존한다.

**확인 시나리오:**
- CLI로 행을 넣고, 고치고, 지운 뒤 `SELECT *`가 id 순서로 바뀐 결과를 보여 준다. 다시 실행해도 같다.
- 가장 큰 id를 지운 뒤 INSERT가 그보다 큰 id를 받는다. 다시 실행해도 같다.
- 조건에 맞는 행이 없으면 `UPDATE 0`이고 파일이 바뀌지 않는다.
- 테스트 빌드에서 임시 파일 쓰기 실패를 주입하면 오류로 끝나고, 원래 파일·조회 결과가 그대로이며 `users.tsv.tmp`가 남지 않는다.

**파일:** `src/storage.{h,c}`, `src/executor.{h,c}`, `src/db.c`, `src/main.c`, `tests/test_unit.c`, `tests/cli_test.py`.

**구현 메모 (2026-11-18):**
- `table_rewrite(t, match, ctx, upd, &count, err)`: design.md "다시 쓰기 순서" 그대로. 원래 파일은 `scan_file`(위치 지정 읽기)로 처음부터 읽고, 행마다 `match`로 대상을 골라 `users.tsv.tmp`에 쓰면서 새 B+ 트리를 만든다. POSIX는 바꿔치기 전에 tmp를 `"a+b"`로 열어 두고 `rename` 뒤에는 실패할 단계가 없다. Windows는 옛 FILE*를 닫고 `MoveFileExA(..., MOVEFILE_REPLACE_EXISTING)` 뒤 다시 연다. 바꿔치기 실패면 원래 파일을 다시 열고 STORAGE, 바꿔치기 뒤 다시 열기 실패면 `DBE_REOPEN_FAILED`.
- Executor: SET 값 검증(INSERT와 같은 `validate_name`·`validate_age`) → SELECT와 같은 경로(`run_where`)로 대상 세기 → 1행 이상이면 `table_rewrite`(대상 판정은 선형 탐색과 같은 `row_matches`). 결과는 새 `ResultSink.on_change(kind, count, ctx)`.
- 다음 id: `Table.last_id`의 뜻이 "파일의 최대 id"에서 "마지막으로 부여한 id"로 넓어졌다. 시작 시 `max(파일의 최대 id, users.next)`. `users.next`는 새 파일의 최대 id가 `last_id`보다 작고 아직 그 값을 적지 않았을 때만 `users.next.tmp` + 바꿔치기로 쓴다. 형식이 틀린 `users.next`는 시작 거부("다음 id 파일 손상").
- 시작 시 남은 `users.tsv.tmp`·`users.next.tmp`를 지운다.
- 새 오류 종류 `DBE_REOPEN_FAILED`와 엔진 상태 `DB_HALTED_REOPEN`(Windows 전용, 서버 사유 코드 `reopen_failed`). 그 밖의 UPDATE·DELETE 실패는 원래 상태가 남으므로 엔진 상태를 바꾸지 않는다.
- design.md 표의 `set_name`·`has_set_age` 대신 P4-01에서 정한 `name`·`age` + `set_cols`를 그대로 쓴다.
- 테스트 빌드 전용 실패 주입: `table_test_fail_rewrite(REWRITE_FAIL_WRITE / NEXT / RENAME / REOPEN)`.

### P4-03 — 서버와 동시성

**흐름:** 서버가 UPDATE·DELETE 결과를 JSON으로 돌려준다. 결과 자리를 실행 전에 확보한다. 동시 정확성 테스트에 UPDATE·DELETE를 섞는다.

**확인 시나리오:**
- `curl`로 UPDATE·DELETE를 보내고 `{"type":"update","count":n}`을 받는다. 이어서 SELECT로 바뀐 결과를 본다.
- 여러 클라이언트가 INSERT·UPDATE·DELETE·SELECT를 섞어 보내도 최종 결과가 각 클라이언트의 기록과 맞고, 재시작 뒤에도 같다.
- `make tsan` 보고 없음.

**파일:** `src/server.c`, `tests/server_test.py`, `tests/test_unit.c`(동시 정확성).

**구현 메모 (2026-11-18):** `json_on_change`가 `{"type":"update"|"delete","count":n}`을 쓴다. INSERT처럼 실행 전에 `jb_reserve(CHANGE_RESULT_MAX = 48)`로 결과 자리를 확보한다: 자리가 없으면 그 문장은 실행하지 않고 `RESULT_TOO_LARGE`(응답에 빠진 UPDATE가 파일에만 반영되는 일이 없다). `DBE_REOPEN_FAILED`는 500 `STORAGE`로 분류한다(POSIX 서버에서는 나오지 않음).

### P4-04 — 문서와 실습

**흐름:** walkthrough에 Phase 4 실습(CLI·`curl`로 UPDATE·DELETE, 다음 id, 실패 시 무변경)과 요청이 엔진까지 가는 지도의 바뀐 부분을 더한다. 100만 행 측정은 하지 않는다(사용자 결정).

**확인 시나리오:** walkthrough의 명령을 그대로 실행해 문서의 응답과 같은지 본다. 파일 다시 쓰기 방식과 덧붙이기 방식(채택하지 않음)의 차이를 설명으로 읽는다.

**파일:** `docs/phase4/walkthrough.md`, README, `examples/phase4_*.sql`.

**구현 메모 (2026-11-18):** walkthrough의 CLI 출력은 Windows 빌드로, 서버 출력은 Docker `gcc:13`에서 실제로 실행해 받은 값이다. 실습마다 "왜"와 "알 수 있는 것"을 적고 필수·선택을 표시했다.

## P4-02~P4-04 검증 기록 (2026-11-18)

| 명령 | 환경 | 결과 |
|---|---|---|
| `mingw32-make`, `mingw32-make test` | Windows, TDM-GCC 10.3 | 경고 0, 단위 31,227개·CLI 56개 실패 0 (Windows 전용 다시 열기 실패 테스트 포함) |
| `make clean && make test` | Docker `gcc:13`, 저장소를 컨테이너 안에 복사 | 경고 0, 단위 31,224개·CLI 56개·서버 27개·부하 생성기 9개 실패 0 |
| `make sanitize` | 같은 환경, ASan/UBSan/LSan | 실패 0, 보고 없음 |
| `make tsan` | 같은 환경, TSan | 단위·서버 실패 0, 보고 없음 |
| 변이 15개 | 같은 환경 | 모두 잡힘(아래) |

변이: 0행이어도 다시 쓰기, DELETE가 행을 남김, UPDATE가 조건 무시, 인덱스 교체 안 함, `file_size` 갱신 안 함, `users.next` 안 씀, 시작 시 `users.next` 무시, 손상된 `users.next` 허용, 실패 시 tmp 남김, 시작 시 tmp 정리 안 함, STORAGE 실패에 중단 상태, SET age 검증 안 함, 서버 결과 자리 확보 안 함(서버 테스트), CLI 행 수 틀림(CLI 테스트), UPDATE·DELETE가 공유 잠금(TSan).

**환경 관찰 — Docker Desktop(Windows) 공유 폴더:** 저장소를 `-v`로 마운트한 채 `make test`를 돌리면 단위 테스트 2개가 매번 실패했다. 다시 쓰기 뒤 Table이 파일을 연 채로 있는 동안 테스트가 같은 경로를 `fopen`하면 "No such file or directory"가 났다. 짧은 재현(파이썬: 열린 파일을 rename한 뒤 새 이름으로 열기)에서 공유 폴더만 실패하고 컨테이너 자체 `/tmp`는 성공했다. Windows 쪽에서는 보였다. 엔진 동작이 아니라 공유 폴더의 동작으로 판단해, Docker 검증은 저장소를 컨테이너 안에 복사해서 했다(`cp -r /src /w && cd /w && make test`). Phase 3까지의 테스트는 rename을 쓰지 않아 이 현상이 없었다.

### Codex 검증 지적 수정 (2026-11-18)

| 지적 | 수정 | 회귀 테스트 |
|---|---|---|
| [P2] `users.next`가 `31 00 30 30 0a`(1, NUL, 00, LF)이면 시작에 성공하고 값 1로 읽어 다음 INSERT에 id 2를 줌. `parse_digits`가 NUL에서 멈추기 때문 | `read_next_file`이 LF 앞의 읽은 바이트 전체가 숫자인지 먼저 확인 | 단위 `test_p4_next_id` 손상 사례를 길이 포함 14가지로(내장 NUL 3가지, 공백·`+`·CR 포함), 거부 시 `users.tsv`·`users.next` 바이트 무변경 확인. CLI `p4_corrupt_next_file_refuses_start`에 같은 바이트, 거부 뒤 INSERT 미실행 확인. 고치기 전 코드로 되돌리면 단위·CLI 모두 실패(변이로 확인) |
| [P2] "UPDATE·DELETE는 다시 보내도 안전하다"는 설명이 틀림: DELETE 성공 → 다른 요청이 조건에 맞는 행 추가 → 재시도가 새 행도 지움. 재실행 행 수도 달라짐 | spec "실패와 재시도"를 경우별로 다시 씀(사이에 쓰기 없음: 데이터는 같고 행 수는 달라질 수 있음 / 끼어든 쓰기: 새 데이터를 바꿀 수 있음 → 먼저 SELECT로 확인). walkthrough 7장의 설명을 고치고 재현 실습(선택) 추가, design D06의 "같은 수" 주장 정정 | 서버 `retried_delete_can_hit_rows_added_in_between` |

검증(수정 후): Windows 단위 31,261개·CLI 56개 / Docker(컨테이너 안 복사) 단위 31,258개·CLI 56개·서버 28개·부하 생성기 9개 실패 0, sanitize·TSan 보고 없음. walkthrough의 새 실습 출력은 Docker 서버에서 실제로 받은 값.

spec 대응:

| ID | 테스트 |
|---|---|
| D2 | 단위 `test_p4_update_delete_basic`(id·age·범위 조건, 여러 행, 값이 같아도 셈, 새 인덱스로 조회·범위 조회, 트리 불변식, 이어지는 INSERT, 재시작), `test_p4_zero_rows_untouched`(0행이면 다시 쓰지 않음: 실패를 걸어 두고 성공 확인), `test_p4_update_validation`(값 검증 실패 시 무변경, SET에 없는 컬럼은 검사 안 함, Storage 이중 검사), `test_p4_model_random`(무작위 60번 vs 모델). CLI `p4_update_delete_basic`, `p4_zero_rows_leave_file_untouched`(inode·수정 시각), `p4_value_errors_change_nothing`, `p4_fail_fast_keeps_earlier_change`, `p4_timing_lines`, `p4_random_model`(25번, 매번 재시작) |
| D3 | 단위 `test_p4_next_id`(가장 큰 id 삭제·재시작·전부 삭제·`users.next`가 더 작을 때·손상 8가지). CLI `p4_delete_max_id_keeps_next_id`, `p4_corrupt_next_file_refuses_start` |
| D4 | 단위 `test_p4_rewrite_failure_keeps_original`(임시 파일 쓰기·users.next·바꿔치기 실패 × DELETE·UPDATE: 파일·조회·트리·엔진 상태 그대로, tmp 없음, 다시 실행하면 성공), `test_p4_windows_reopen_failure_halts`(Windows), `test_p4_startup_removes_tmp`. CLI `p4_stale_tmp_removed_on_start` |
| D5 | 서버 `update_delete_json_results`(JSON 결과, 실행 오류 시 앞 문장 유지, 다음 id, 재시작 뒤 CLI로 같은 내용), `update_result_space_reserved_before_execution`, `update_delete_syntax_errors_execute_nothing`, `concurrent_mixed_writes_over_http`(8 클라이언트). 단위 `test_p4_concurrent_mixed`(4 스레드 × 150 연산, id 중복·빠짐 없음, 재시작 두 번). `make tsan` |
| D6 | `docs/phase4/walkthrough.md`, README |
| D7 | 위 검증 기록의 전체 테스트·sanitize·TSan |

## 공통 규칙 (Phase 1~3에서 이어짐)

- 경고 0으로 빌드한다(`-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes`).
- 테스트를 추가하면 변이 테스트로 확인한다.
- Windows `mingw32-make test`가 계속 통과해야 한다.
- 직접 잰 값과 추정을 구분해 적는다. 보장하지 않는 것을 보장한다고 쓰지 않는다.
