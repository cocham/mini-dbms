# Phase 5 구현 순서 — 커서 페이지 조회

## 현재 상태

**P5-01~P5-03 구현 완료(2026-11-18), 사용자 검증 대기.** P5-00 확정(2026-11-18, 사용자). 브랜치 `phase5-pagination`(main `4d75692`에서 시작). D01~D11 확정. D04(순방향 조회로 보장 한정), D08(최악 크기 반영, 기본 limit도 상한 이하, 실패 방지는 결과 크기 상한에 한정), D10(변경 없는 100만 행에서 세 가지 확인)은 사용자가 범위를 고쳤다.

| ID | 기능 | 완료 기준 | 상태 |
|---|---|---|---|
| P5-00 | 구현 전 결정 | design.md DB-P5-D01~D11 확정 | 완료(2026-11-18) |
| P5-01 | 엔진 제한 읽기 | E1, E5(단위) | 완료(검증 대기) |
| P5-02 | `GET /users` | E2, E3, E4, E5(서버) | 완료(검증 대기) |
| P5-03 | 100만 행 확인, 문서·실습 | E6, E8 | 완료(검증 대기) |

진행 방식(D11): P5-01~P5-03을 이어서 구현·자체 검증·커밋하고 한 번에 검증받는다. 단위마다 그 단위의 테스트(단위·서버, 해당하면 sanitize·TSan)를 실행해 통과시킨 뒤 다음 단위로 간다. PASS 뒤 push와 main 대상 PR.

## 단위별 흐름과 확인 시나리오

### P5-01 — 엔진 제한 읽기

**흐름:** `table_page(t, after_id, limit, fn, ctx, &has_more, err)`가 B+ 트리로 `after_id + 1` 이상인 첫 키를 찾고, 리프 연결을 따라 최대 `limit + 1`개의 키를 확인한다. 앞의 `limit`행만 위치 지정 읽기로 읽어 `fn`에 넘기고, `limit + 1`번째 키가 있으면 그 줄은 읽지 않고 `has_more = 1`로 표시한 뒤 정상으로 멈춘다. `db_page`는 이것을 공유 잠금 안에서 부른다.

**확인 시나리오:**
- id 1~10에서 `limit 3`: 첫 페이지 `[1,2,3]`과 `has_more 1`. `after_id 9`면 `[10]`과 `has_more 0`.
- 정확히 `limit`행이 남은 경우(id 1~6, `after_id 3`, `limit 3`): `[4,5,6]`, `has_more 0`.
- 빈 테이블, `after_id ≥ 최대 id`, `after_id = INT64_MAX`: 행 없음, `has_more 0`, 오류 없음.
- id 3·4·5를 지운 뒤 `limit 3`: `[1,2,6]` → `[7,8,9]` → `[10]`. 지워진 id(예: 4)를 `after_id`로 줘도 `[6,7,8]`.
- 테스트 빌드의 "파일에서 읽은 줄 수"가 `min(limit, 남은 행)`과 같다. 100만 행이 아니라도(예: 1만 행) "첫 페이지에 3줄만 읽었다"로 조기 종료를 확인할 수 있다.
- UPDATE·DELETE의 다시 쓰기 뒤에도 같은 페이지 결과(파일 위치가 바뀌어도 id 커서는 그대로).
- 동시성: 한 스레드가 `db_page`로 순회하는 동안 다른 스레드들이 INSERT·UPDATE·DELETE. design.md "동시성"의 (가)(나)(다), TSan 보고 없음.

**파일:** `src/storage.{h,c}`, `src/db.{h,c}`, `tests/test_unit.c`.

**구현 메모 (2026-11-18):**
- `range_visit`를 `table_find_id_range`와 함께 쓴다. `RangeRead`에 `limit`·`lines`·`has_more`를 더하고, `limit`개를 넘긴 뒤 키를 하나 더 받으면 그 줄은 읽지 않고 `RANGE_STOP`(2)을 돌려준다. `bpt_range`가 그 값을 그대로 돌려주므로 `range_read`가 0·`RANGE_STOP`을 성공으로 본다. B+ 트리 코드는 바꾸지 않았다.
- N+1번째는 인덱스 키로만 확인한다(파일 줄은 `limit`개만 읽음). spec·design의 "limit+1행을 읽는다"는 문구를 "limit+1개의 키를 확인하고 줄은 limit개만 읽는다"로 고쳤다.
- 테스트 빌드의 `table_test_page_lines_read()`는 `_Thread_local` 변수라 동시 테스트에서도 경쟁이 없다.
- `table_page`는 `limit < 1`·`after_id < 0`을 `DBE_SQL`로 거부한다(서버가 먼저 검증하므로 방어용).

**P5-01 단계 테스트 (2026-11-18):** Windows 단위 41,734개 실패 0(10회 연속). Docker `gcc:13` 단위 41,731·CLI 56·서버 28·부하 생성기 9 실패 0, sanitize·TSan 보고 없음.

**같이 고친 Phase 4 문제:** Windows에서 `test_p4_concurrent_mixed`(그리고 새 `test_p5_concurrent_forward_walk`)가 약 절반 확률로 "데이터 파일 바꿔치기 실패 (Permission denied)"로 실패했다. 고치기 전 코드에서도 8회 중 4회 재현됐다. `GetLastError`로 확인하니 `MoveFileExA`의 `ERROR_ACCESS_DENIED`(5)였다. 방금 쓰거나 닫은 파일을 다른 프로세스(백신 검사·색인 등)가 잠깐 여는 경우로 보고(추정), 이 오류와 `ERROR_SHARING_VIOLATION`에 한해 최대 약 1초 다시 시도하게 했다(커밋 `9dfd3a0`, Phase 4 동작 수정). POSIX는 바뀌지 않는다.

### P5-02 — `GET /users`

**흐름:** HTTP 계층이 쿼리 문자열을 보관한다. 서버는 `/users` GET 요청의 파라미터를 검증하고, `db_page`로 페이지를 읽어 JSON으로 답한다. 시작할 때 페이지 상한을 계산해 시작 줄에 찍는다.

**확인 시나리오:**
- `GET /users` → 기본값(`after_id 0`, `limit 100`)으로 첫 페이지. 응답 형식은 spec "응답"과 같다.
- spec "입력 검증" 표의 모든 행이 400 `INVALID_PARAM`이고, 엔진이 불리지 않는다. 확인 방법: 서버 로그, 또는 테스트 빌드의 호출 횟수.
- `POST /users` → 405, `Allow: GET`. 중단 상태에서 `GET /users` → 503 `HALTED`.
- 큰 데이터(E3): `--max-rows`를 작게(예: 1000) 띄우고 5000행을 넣으면 `SELECT * FROM users;`는 `RESULT_TOO_LARGE`다. 같은 서버에서 `limit=1000`으로 페이지를 따라가면 5000행 전부를 id 순서로, 중복·누락 없이 받는다(5페이지, 마지막 페이지 `has_more false`).
- 페이지 상한(E4): 기본 설정 1000(기본 limit 100), `--max-rows 50`이면 50(기본 50), `--max-response 4096`이면 2(기본 2). 상한 + 1은 400이고 메시지에 상한값이 나온다. 이름이 255바이트 제어 문자(최악의 이스케이프)인 행으로 페이지 상한만큼 받아도 200이고 본문이 `--max-response` 이하다.
- 동시 순회(E5): HTTP로 `limit`을 작게 해서 커서 0부터 순방향으로 순회하는 동안 다른 클라이언트들이 INSERT·UPDATE·DELETE. (가)(나)(다) 확인.
- `/query` 응답은 바뀌지 않는다(기존 서버 테스트 전부 통과).

**파일:** `src/http.{h,c}`, `src/server.c`, `tests/server_test.py`.

**구현 메모 (2026-11-18):**
- `HttpRequest`에 `query`(최대 256바이트)와 `query_over`를 더했다. 넘쳐도 HTTP 단계에서는 거절하지 않고(`/query?…` 등 기존 경로는 쿼리를 쓰지 않으므로 동작 그대로), `/users`가 400 `INVALID_PARAM`으로 거절한다.
- 페이지 상한은 `main`에서 옵션을 읽은 뒤 한 번 계산한다: `min(1000, --max-rows, (--max-response − 102) ÷ 1559)`. `--max-response 4096`이면 2. 시작 줄 끝에 `페이지 상한 N행(기본 M)`을 찍는다.
- 행 JSON은 `/query`와 같은 함수(`json_row`)로 만든다. `/query`의 바이트 결과는 그대로다(기존 서버 테스트 전부 통과).
- 최악의 페이지(19자리 id, 이름 255바이트 ``, age 150, 2행)는 실제로 3,219바이트였다. 계산한 상한 102 + 1,559×2 = 3,220 이하다.
- 처리 순서: 입력 검증 → `db_page`. 중단 상태에서도 잘못된 파라미터는 400이다(엔진을 부르기 전에 끝남).

**P5-02 단계 테스트 (2026-11-18):** Docker `gcc:13` 단위 41,731·CLI 56·서버 34(새 6개)·부하 생성기 9 실패 0, sanitize·TSan 보고 없음. Windows 단위 41,734·CLI 56 실패 0.

### P5-03 — 100만 행 확인, 문서·실습

**100만 행 확인(E6, D10):** 변경 없는 100만 행(`tools/gen_sql.py`로 만든 데이터)을 띄운 서버에서 세 가지를 확인하고 `docs/phase5/benchmark.md`에 기록한다. 계측값과 추정을 구분한다.
1. `SELECT * FROM users;`가 `RESULT_TOO_LARGE`(400)로 실패한다. 걸린 시간과 응답 바이트도 적는다.
2. `limit=1000`으로 커서 0부터 `has_more: false`까지 따라가면 1000페이지에 100만 행을 받는다. 받은 행이 `users.tsv`와 순서·값까지 같다(중복·누락 없음).
3. 첫·중간·마지막 페이지(`limit` 100과 1000)의 응답 시간과 응답 바이트. 여러 번 재서 중앙값을 적는다.

측정 도구는 Phase 3처럼 컨테이너 안 임시 폴더를 쓴다. 측정 스크립트는 `tools/`에 둔다.

**문서 흐름:** `docs/phase5/walkthrough.md`에 직접 해 보는 실습을 쓴다. 실습마다 "왜 / 알 수 있는 것"과 필수·선택 표시를 단다. 출력은 실제로 실행해 받은 값만 쓴다.

**실습 후보:**
- (필수) `SELECT *`가 `RESULT_TOO_LARGE`로 실패하는 것을 먼저 본다 → 같은 데이터를 `GET /users`로 끝까지 받는다.
- (필수) id 공백: 몇 행을 지우고 페이지가 덜 차지 않는 것을 본다. `BETWEEN`으로 나눠 받을 때와 비교한다.
- (필수) 마지막 페이지: 정확히 `limit`행이 남았을 때 `has_more`가 `false`인 것(N+1의 효과).
- (선택) 순회 도중 INSERT·DELETE를 끼워 넣고 spec "동시 변경과 보장" 표대로인지 본다.
- (선택) 커서가 바이트 위치가 아니라 id인 이유: 페이지 사이에 UPDATE로 파일 전체가 다시 쓰여도 다음 페이지가 그대로 이어지는 것.

README에 Phase 5 절을 더한다.

## P5-03 기록 (2026-11-18)

- `tools/page_bench.py`, `make page-bench`. 결과는 [benchmark.md](benchmark.md): 전체 조회 400 `RESULT_TOO_LARGE`(146바이트), 1000행씩 1000페이지에 100만 행·파일과 다른 행 0, 페이지 하나 0.3~0.6 ms(첫·중간·마지막 같은 수준). 두 번 실행.
- `--rows`가 100,000 이하이면 전체 조회가 기본 상한에 걸리지 않는다. 그래서 `--max-rows`를 rows/2로 낮춰 띄운다(작은 데이터로 도구를 확인할 때 쓰는 용도). 2만 행으로 도구 동작을 확인했다.
- `docs/phase5/walkthrough.md`: 필수 4개·선택 4개. 출력은 Docker 서버에서 실제로 받은 값이다. 실습 5(순회 중 변경)는 처음에 커서를 건너뛴 순서(`after_id=7`)로 실행했다. 순방향 조회가 아니어서 받은 커서를 그대로 넘기는 순서로 다시 실행해 그 출력을 넣었다.
- README에 Phase 5 절을 더했다. Phase 4 상태를 "완료"로 고쳤다.
- 변이 테스트 중 "멈추지 않고 키를 끝까지 훑는" 변이(`RANGE_STOP` 대신 0)가 결과·읽은 줄 수로는 드러나지 않았다. 그래서 테스트 빌드에 "리프에서 받은 키 수"(`table_test_page_keys_seen`, `min(limit+1, 남은 행)`)를 더해 잡히게 했다.

## 검증 기록 (2026-11-18, P5-01~P5-03 합본)

| 명령 | 환경 | 결과 |
|---|---|---|
| `mingw32-make`, `mingw32-make test` | Windows, TDM-GCC 10.3 | 경고 0, 단위 41,735개·CLI 56개 실패 0 |
| `make clean && make test` | Docker `gcc:13`, 컨테이너 안 복사본 | 경고 0, 단위 41,732개·CLI 56개·서버 34개·부하 생성기 9개 실패 0 |
| `make sanitize` | Docker `gcc:13`, ASan/UBSan/LSan | 실패 0, 보고 없음 |
| `make tsan` | Docker `gcc:13`, TSan | 단위·서버 실패 0, 보고 없음 |
| `make page-bench` | Docker `gcc:13`, 100만 행 | 확인 3가지 통과(두 번), benchmark.md |
| 변이 18개 | Docker `gcc:13` | 모두 잡힘(아래) |

변이 18개(각각 단위 테스트, `users_` 서버 테스트 6개, 또는 TSan 단위 테스트로 확인):

| 변이 | 잡은 곳 |
|---|---|
| `has_more`를 `읽은 줄 == limit`으로 | 단위(정확히 limit행 남음, 전체 순회 페이지 수) |
| `limit+1`번째 행을 응답에 넣음 | 단위 |
| 끝까지 읽고 `limit`개만 넘김 | 단위(읽은 줄 수) |
| 멈추지 않고 키를 끝까지 훑음 | 단위(받은 키 수) |
| 정상 멈춤을 오류로 | 단위 |
| 커서 포함(`>=`) | 단위 |
| `INT64_MAX` 넘침 처리 제거 | 단위·서버 |
| 모르는 파라미터 무시 / 중복 허용 / `limit=0` 허용 / 앞자리 0 허용 / 끝의 `&` 허용 | 서버 |
| 페이지 상한이 바이트를 무시(1000 고정) / 기본 limit이 상한을 넘음 | 서버 |
| 빈 페이지 `next_cursor`를 0으로 / `next_cursor`가 첫 행 id | 서버 |
| 긴 쿼리 문자열 허용 | 서버 |
| `db_page`가 잠금 없이 읽음 | TSan(단위 동시 순회, `fclose`와의 경쟁) |

spec 대응:

| ID | 테스트 |
|---|---|
| E1 | 단위 `test_p5_page_basic`, `test_p5_page_gaps_and_rewrite`, `test_p5_page_early_stop_and_full_walk`, `test_p5_db_page_halted` |
| E2 | 서버 `users_pages_basic`, `users_invalid_params`(거부 34가지·허용 경계·405·404 안내), `users_halted_and_validation_order` |
| E3 | 서버 `users_walks_data_that_select_star_cannot_return`(5000행, `--max-rows 1000`) |
| E4 | 서버 `users_page_limit_fits_response_budget`(1000·50·2, 최악의 행 3,219바이트) |
| E5 | 단위 `test_p5_concurrent_forward_walk`(TSan 포함), 서버 `users_forward_walk_during_writes` |
| E6 | `make page-bench`, benchmark.md |
| E7 | 기존 테스트 전부(위 표) |
| E8 | walkthrough.md, README |

## 검증 계획

| 명령 | 환경 |
|---|---|
| `mingw32-make`, `mingw32-make test` | Windows, TDM-GCC 10.3 (단위·CLI. 서버는 POSIX 전용) |
| `make clean && make test` | Docker `gcc:13`, 컨테이너 안 복사본(Phase 4의 bind mount 관찰) |
| `make sanitize`, `make tsan` | Docker `gcc:13` |
| 변이 테스트 | 아래 목록. 각각 테스트가 잡아야 한다 |

변이 후보:
- N+1을 하지 않고 `has_more = (받은 행 수 == limit)`으로 판단 → "정확히 limit행 남음"에서 잡혀야 한다.
- `limit + 1`번째 행을 응답에 넣음
- 조기 종료를 하지 않고 끝까지 읽음 → 읽은 행 수에서 잡혀야 한다.
- 조기 종료를 오류(`DBE_SINK`)로 처리
- `after_id` 이상(`>=`)부터 줌 → 커서 행이 두 번 나온다
- `after_id = INT64_MAX`의 넘침 처리 제거
- 모르는 파라미터 무시, 중복 파라미터 허용, `limit=0` 허용
- 페이지 상한을 계산하지 않고 1000 고정 → `--max-response 4096`에서 잡혀야 한다.
- 빈 페이지의 `next_cursor`를 0으로
- `db_page`가 잠금 없이 읽음 → TSan에서 잡혀야 한다.

## 학습과의 분리

구현·검증 순서는 위와 같다. 사용자가 직접 해 보는 실습은 P5-03 walkthrough에 따로 둔다. 실습은 구현을 검증하는 수단이 아니다.
