# Phase 4 구현 순서 — UPDATE·DELETE

## 현재 상태

**P4-00 확정(2026-11-18).** 브랜치 `phase4-update-delete`(`phase3-server`의 `f8f88b4`에서 시작). D01~D08 모두 확정. 100만 행 측정은 하지 않기로 했다(사용자). 다음은 P4-01.

진행 방식: 한 단위를 구현한다 → 테스트와 ASan·TSan 검사로 검증한다 → 통과하면 커밋한다 → 사용자가 검증 결과(PASS)를 전하면 push·main 대상 PR을 만들고 다음 단위로 간다. 사용자 동작이나 공개 인터페이스를 바꾸는 결정은 구현 전에 짧게 제안한다.

| ID | 기능 | 완료 기준 | 상태 |
|---|---|---|---|
| P4-00 | 구현 전 결정 | design.md DB-P4-D01~D08 확정 | 완료(2026-11-18) |
| P4-01 | 문법 | D1 | 대기 |
| P4-02 | 파일 다시 쓰기와 CLI | D2, D3, D4 | 대기 |
| P4-03 | 서버와 동시성 | D5 | 대기 |
| P4-04 | 문서와 실습 | D6 | 대기 |

## 단위별 흐름과 확인 시나리오

### P4-01 — 문법

**흐름:** Parser가 `UPDATE ... SET ... WHERE ...;`와 `DELETE FROM ... WHERE ...;`를 `Statement`로 만든다. 실행은 아직 하지 않는다. CLI·서버는 이 문장을 받으면 "아직 지원하지 않는 문장"(실행 오류, 아무것도 바뀌지 않음)으로 답한다.

**확인 시나리오:**
- `UPDATE users SET age = 31 WHERE id = 3;`는 파싱되고, CLI는 "아직 지원하지 않음" 오류로 끝난다.
- `DELETE FROM users;`(WHERE 없음), `UPDATE users SET id = 5 WHERE id = 1;`, `UPDATE users SET age = 1, age = 2 WHERE id = 1;`, `EXPLAIN DELETE FROM users WHERE id = 1;`은 구문 오류이고, 서버에서는 같은 요청의 앞 문장도 실행되지 않는다(전체 선파싱).

**파일:** `src/parser.{h,c}`, `src/executor.c`(미지원 오류), `tests/test_unit.c`, `tests/cli_test.py`.

### P4-02 — 파일 다시 쓰기와 CLI

**흐름:** Executor가 값을 검증하고 `table_rewrite`로 임시 파일에 새 내용을 쓴 뒤 바꿔치기한다. 새 인덱스로 교체한다. CLI는 `UPDATE n`·`DELETE n`을 출력한다. 다음 id는 `users.next`로 보존한다.

**확인 시나리오:**
- CLI로 행을 넣고, 고치고, 지운 뒤 `SELECT *`가 id 순서로 바뀐 결과를 보여 준다. 다시 실행해도 같다.
- 가장 큰 id를 지운 뒤 INSERT가 그보다 큰 id를 받는다. 다시 실행해도 같다.
- 조건에 맞는 행이 없으면 `UPDATE 0`이고 파일이 바뀌지 않는다.
- 테스트 빌드에서 임시 파일 쓰기 실패를 주입하면 오류로 끝나고, 원래 파일·조회 결과가 그대로이며 `users.tsv.tmp`가 남지 않는다.

**파일:** `src/storage.{h,c}`, `src/executor.{h,c}`, `src/db.c`, `src/main.c`, `tests/test_unit.c`, `tests/cli_test.py`.

### P4-03 — 서버와 동시성

**흐름:** 서버가 UPDATE·DELETE 결과를 JSON으로 돌려준다. 결과 자리를 실행 전에 확보한다. 동시 정확성 테스트에 UPDATE·DELETE를 섞는다.

**확인 시나리오:**
- `curl`로 UPDATE·DELETE를 보내고 `{"type":"update","count":n}`을 받는다. 이어서 SELECT로 바뀐 결과를 본다.
- 여러 클라이언트가 INSERT·UPDATE·DELETE·SELECT를 섞어 보내도 최종 결과가 각 클라이언트의 기록과 맞고, 재시작 뒤에도 같다.
- `make tsan` 보고 없음.

**파일:** `src/server.c`, `tests/server_test.py`, `tests/test_unit.c`(동시 정확성).

### P4-04 — 문서와 실습

**흐름:** walkthrough에 Phase 4 실습(CLI·`curl`로 UPDATE·DELETE, 다음 id, 실패 시 무변경)과 요청이 엔진까지 가는 지도의 바뀐 부분을 더한다. 100만 행 측정은 하지 않는다(사용자 결정).

**확인 시나리오:** walkthrough의 명령을 그대로 실행해 문서의 응답과 같은지 본다. 파일 다시 쓰기 방식과 덧붙이기 방식(채택하지 않음)의 차이를 설명으로 읽는다.

**파일:** `docs/phase4/walkthrough.md`, README.

## 공통 규칙 (Phase 1~3에서 이어짐)

- 경고 0으로 빌드한다(`-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes`).
- 테스트를 추가하면 변이 테스트로 확인한다.
- Windows `mingw32-make test`가 계속 통과해야 한다.
- 직접 잰 값과 추정을 구분해 적는다. 보장하지 않는 것을 보장한다고 쓰지 않는다.
