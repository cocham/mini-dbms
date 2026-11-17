# Phase 3 구현 순서

## 현재 상태

**P3-05 완료(2026-11-17) — Phase 3의 모든 단위 구현 완료.** 부하 측정 결과는 [benchmark.md](benchmark.md), 실습은 [walkthrough.md](walkthrough.md) 8장. P3-04는 사용자 검증 PASS(2026-11-17, `c8ad6d3`). P3-05는 사용자 검증(PASS)을 기다린다.

진행 방식: 한 단위를 구현한다 → 테스트와 ASan·TSan 검사로 검증한다 → 통과하면 커밋한다 → **사용자가 검증 결과(PASS)를 전하면** 다음 단위로 간다(2026-11-16 사용자 지시, P3-01부터). 사용자 동작이나 공개 인터페이스를 바꾸는 결정은 구현 전에 짧게 제안한다.

| ID | 기능 | 완료 기준 | 상태 |
|---|---|---|---|
| P3-00 | 구현 전 결정 | design.md DB-P3-D01~D11 확정 | 완료(2026-11-16) |
| P3-01 | 엔진 진입점과 전역 잠금 | S1, S1x | 완료 |
| P3-02 | 단일 스레드 HTTP 서버 | S2, 실행 예시와 요청 경로 지도 | 완료 |
| P3-03 | 스레드 풀과 작업 큐 | S3 | 완료 |
| P3-04 | 읽기 병렬화 | S4 | 완료 |
| P3-05 | 부하 측정과 마무리 | S5 | 완료 |

## 학습과의 분리

- 구현·검증은 위 순서대로 진행한다. 사용자의 학습은 그 결과물에서 따로 시작하며, 구현의 진행 조건이 아니다.
- 함수별 학습 노트는 만들지 않는다. 단위마다 **전체 흐름**과 **사용자가 확인할 시나리오**만 짧게 남긴다(아래).
- 잠금·스레드 풀 이론을 먼저 설명하지 않는다. **P3-02가 끝나면** 작동하는 서버를 바로 실행해 볼 수 있게 `docs/phase3/walkthrough.md`를 준다. 여기에는 `curl` 실행 예시와 기대 응답, 그리고 요청 하나가 소켓에서 기존 엔진(`executor_run` → `table_find_by_id` → B+ 트리 → `users.tsv`)까지 가는 전체 지도(파일·함수 이름 포함)가 들어간다. 이후 단위는 이 지도에 바뀐 부분만 덧붙인다.

## 단위별 흐름과 확인 시나리오

### P3-01 — 엔진 진입점과 전역 잠금

**흐름:** CLI는 문장마다 `parser_next`를 거쳐 `db_exec`(잠금 → `executor_run` → 잠금 해제)를 부른다. 서버는 아직 없다. 바뀌는 곳은 엔진 앞의 한 겹(`db.c`)과 `table_append` 실패 구분, `scan_file`의 `last_id` 분리다.

**확인 시나리오:**
- 기존 예제와 CLI 테스트의 출력이 바이트 단위로 같다. 사용자 입장에서는 달라진 것이 없어야 한다.
- `make race-demo`로 잠금을 끈 빌드를 돌려 id 중복 같은 이상이 나타나는 모습을 본다(나타나지 않는 실행도 있을 수 있다).
- 같은 작업을 정상 빌드로 돌리면 id가 1..M으로 이어진다.

**파일:** 새 `src/db.{h,c}`, `tools/race_demo.c`. 바뀌는 곳: `src/common.{h,c}`(오류 종류 `DbErrorKind`, `db_error_setk`, `db_errstr`), `src/storage.{h,c}`(append 실패 3가지 구분, 인덱스 중복과 메모리 부족 문구 구분, `last_id` 분리, `table_last_id`, 테스트 전용 쓰기 실패 주입·인덱스 검사), `src/executor.c`(오류 종류 표시), `src/main.c`(진입점 사용), `tests/test_unit.c`, `Makefile`(`-pthread`, `tsan`, `race-demo`).

**구현 메모:**
- `db.c`는 TDM-GCC의 winpthreads로 Windows에서도 빌드된다(정적 링크, 추가 DLL 없음). 그래서 동시 정확성 테스트도 Windows와 Linux 양쪽에서 돈다.
- `scan_file`은 `out_last_id`를 받는다. `table_open`만 넘기고 `table_scan`은 NULL을 넘긴다. 외부 동작은 같아서 이 변경을 단독으로 잡는 테스트는 없다. 효과는 P3-04(공유 잠금 아래 선형 탐색)에서 드러난다.
- 기존 CLI 출력·오류 문구는 바뀌지 않았다. 바뀐 문구는 잠금 없이만 생기는 인덱스 중복 경우("id N가 이미 인덱스에 있어 ...") 하나뿐이다.

**P3-01 검증 기록(2026-11-16)**

| 명령 | 환경 | 결과 |
|---|---|---|
| `mingw32-make`, `mingw32-make test` | Windows, TDM-GCC 10.3 | 경고 0, 단위 26,034개 검사 실패 0, CLI 46개 실패 0 |
| `make sanitize` | Docker `gcc:13`, ASan/UBSan/LSan | 경고 0, 단위·CLI 실패 0, 검사 도구 보고 없음 |
| `docker run --security-opt seccomp=unconfined ... make tsan` | Docker `gcc:13`, TSan | 단위 26,034개 실패 0, TSan 보고 없음(C5) |
| 변이 7개 | Windows | 모두 잡힘: 잠금 제거(정확성 테스트가 실패하거나 프로세스가 죽음), `stored_id` 미설정, `WRITE_FAILED`에 `stored_id` 설정, 인덱스 실패 뒤 중단 안 함, 중단 상태 검사 제거, 쓰기 실패를 `STORAGE`로 분류, 결과 콜백 실패를 `SQL`로 분류 |
| 예제 `phase2_*` | Windows | 정상 |

TSan 메모: GCC 13의 TSan은 최근 커널(mmap 무작위 비트 32)에서 "unexpected memory mapping"으로 시작하지 못한다. 그래서 `tsan`·`race-demo`는 `setarch -R`로 주소 무작위화를 끄고 실행하며, Docker에서는 `--security-opt seccomp=unconfined`가 필요하다.

spec 대응:

| ID | 테스트·기록 |
|---|---|
| S1 | `test_db_concurrent_correctness`: 스레드 8개 × 400개 작업(INSERT 40%, id 조회, BETWEEN, 전체 조회, EXPLAIN). id 1..M 중복·누락 없음(C1), 범위·전체 결과에 빈틈 없음(C2), 방금 넣은 행 즉시 조회(C3), 트리 불변식·행 수·재시작 후 내용 일치(C4). `make tsan` 보고 없음(C5). `test_db_exec_basic_and_kinds`(오류 종류, 선형 탐색 뒤 다음 id), `test_db_halt_after_index_failure`(`stored_id` 64, 중단, 재시작으로 복구), `test_db_halt_after_write_failure`(`stored_id` 없음, 중단, 줄 일부가 남아 재시작 거부). CLI 46개 출력 불변 |
| S1x | `make race-demo` 관찰 기록(아래). 판정에는 쓰지 않는다 |

**S1x 관찰 기록(2026-11-16, Docker `gcc:13`, 잠금 없는 빌드, 스레드 8개):**

| 단계 | 관찰(일반 실행 8번 + TSan 실행 2번) |
|---|---|
| 읽기만(미리 넣은 2,000행을 16,000번 id 조회) | 매번 65~298번 "인덱스와 데이터 파일이 일치하지 않습니다". 공유 `FILE*`의 `fseek`·`getc`가 스레드끼리 섞여 다른 위치의 줄을 읽었고, 읽은 줄의 id 재확인이 엉뚱한 행 대신 오류를 냈다. 파일은 바뀌지 않았다 |
| 동시 INSERT(4,000번) | 매번 처음 몇 건(2~17건) 안에 같은 id를 두 스레드가 받았다. 두 번째 줄은 파일에 기록됐지만 인덱스 등록이 중복으로 실패했고(`INDEX_FAILED`), 엔진은 중단 상태가 되어 나머지 INSERT를 모두 거절했다 |
| 재시작 | 매번 거부. 같은 id의 줄이 두 번 기록되어 "id가 증가 순서가 아닙니다"(데이터 파일 손상) |
| TSan(같은 빌드) | 경고 6개. 모두 `table_append`의 `last_id` 읽기·쓰기(storage.c)다. **읽기 단계의 오류는 TSan이 보고하지 않았다.** stdio 함수는 호출 하나마다 내부 잠금이 있어서 메모리 경쟁은 없지만, `fseek`와 `getc`의 묶음은 원자적이지 않다. TSan이 잡는 것은 메모리 접근 경쟁이고, 이런 논리적 경쟁은 잡지 못한다. 그래서 P3-04의 `pread` 전환은 TSan 결과와 별개로 필요하다 |

**P3-04 뒤 다시 관찰(2026-11-17, 같은 조건, 일반 실행 5번 + TSan 1번):** 읽기 단계는 매번 16,000번 모두 정상, 오류 0. 조회가 공유 `FILE*` 대신 위치를 지정해 읽기(`pread`) 때문이다. 쓰기 단계는 그대로 망가진다(첫 16~17건 뒤 중복 id → 중단, 재시작 거부). 그래서 INSERT에는 여전히 배타 잠금이 필요하고, 읽기끼리만 잠금을 나눠 가질 수 있다.

### P3-02 — 단일 스레드 HTTP 서버

**흐름:** 연결을 받는다 → 요청을 읽는다 → 문장 전체를 파싱한다 → 문장마다 `db_exec`(JSON Sink) → 응답을 보낸다 → 연결을 닫는다. 한 번에 연결 하나씩 처리한다.

**확인 시나리오** (`walkthrough.md`에 명령과 기대 응답을 넣는다):
- `curl`로 INSERT → id 조회 → 범위 조회 → EXPLAIN을 보낸다. CLI 결과와 같은 데이터가 JSON으로 온다.
- 구문 오류가 섞인 요청을 보내면 400 `SQL_SYNTAX`이 오고, 아무것도 저장되지 않았음을 조회로 확인한다.
- 실행 단계 오류에서는 앞 INSERT만 저장되고 `statement` 번호가 온다.
- `--max-rows 3`으로 띄우고 SELECT를 여러 개 보내면 `RESULT_TOO_LARGE`가 오고, 앞 결과만 남는다.
- 서버를 끄고 다시 띄워도 데이터가 그대로다.

**파일:** 새 `src/server.c`, `src/http.{h,c}`, `src/json.{h,c}`, `tests/server_test.py`, `docs/phase3/walkthrough.md`, `examples/phase3_seed.sql`, `examples/phase3_partial.sql`. 바뀌는 곳: `src/executor.{h,c}`(EXPLAIN 문구 `executor_plan_name`·`executor_plan_condition`을 CLI와 서버가 함께 씀), `src/main.c`(그 함수 사용, 출력 불변), `tests/test_unit.c`(JSON 버퍼), `Makefile`(`minidb-server`, 테스트 전용 `minidb-server-test`, `server` 대상. POSIX에서만 `all`·`test`에 들어감).

**구현 메모:**
- 기한(호출 5초, 수신·전송 전체 10초)은 단일 스레드 서버가 연결 하나에 영영 묶이지 않도록 이 단위에서 이미 넣었다. 자동 테스트는 P3-03에서 추가한다. 수동으로는 walkthrough "직접 해 볼 것 5"에서 확인했다(다른 요청이 5.3초 기다린 뒤 응답, 덜 보낸 요청은 408).
- 응답은 머리(`{"ok":...,"results":[`)·결과·`]}` 세 조각으로 보낸다. 결과를 다 만든 뒤에야 성공·실패를 알기 때문이다. 조각을 이어 붙이는 복사는 하지 않는다.
- 저장 후 실패는 테스트 전용 빌드 `minidb-server-test`에서 환경 변수 `MINIDB_TEST_FAULT=index|write`로 재현한다. 배포용 `minidb-server`에는 이 코드가 없다.
- `500 INTERNAL`(응답을 만들 메모리 부족)은 자동 테스트가 없다. 고정 본문을 보내는 경로만 있다.
- spec에 추가한 것: `BAD_REQUEST` 코드, `statement`·`results`를 넣는 조건, 올바르지 않은 UTF-8의 `�` 치환, 예비 공간 2 KiB와 `--max-response` 최솟값 4096, `--port 0`, 로그 형식.

**P3-02 검증 기록(2026-11-16)**

| 명령 | 환경 | 결과 |
|---|---|---|
| `mingw32-make`, `mingw32-make test` | Windows, TDM-GCC 10.3 | 경고 0, 단위 26,052개 검사 실패 0, CLI 46개 실패 0(서버는 빌드하지 않음) |
| `make`, `make test` | Docker `gcc:13` | 경고 0, 단위 26,052개·CLI 46개·서버 15개 실패 0 |
| `make sanitize` | Docker `gcc:13`, ASan/UBSan/LSan | 단위·CLI·서버 실패 0, 검사 도구 보고 없음(서버는 SIGTERM 종료 뒤 누수 검사까지) |
| `docker run --security-opt seccomp=unconfined ... make tsan` | Docker `gcc:13`, TSan | 단위·서버 실패 0, 보고 없음 |
| 서버 변이 8개 | Docker `gcc:13` | 모두 잡힘: 행 예산을 문장마다 초기화, INSERT 자리 선확인 제거, 실패 문장 결과를 되돌리지 않음, `WRITE_FAILED`에 `stored_id`, 파싱하며 실행, 따옴표 이스케이프 안 함, 잘못된 UTF-8 그대로, `Transfer-Encoding` 허용 |
| walkthrough의 명령 전부 | Windows PowerShell `curl.exe` → Docker 서버 | 문서의 응답과 같음. CLI로 서버가 쓴 파일을 읽으면 같은 행 |

**검증 지적 수정(2026-11-17):** 요청 대상에 NUL이 있으면(`POST /query<NUL>/invalid`) 경로를 그대로 복사해 이후 `strcmp`가 `/query`까지만 보고 SQL을 실행했다(200, 데이터 저장). 이제 요청 대상 전체(쿼리 문자열 포함)는 0x21~0x7E만 받고, 헤더 줄은 NUL·제어 문자(탭 제외)를 거부해 400 `BAD_REQUEST`를 돌려준다. 회귀 테스트 `bad_bytes_in_target_or_headers_execute_nothing`: 경로 7가지·헤더 3가지가 모두 400이고, `/health` 행 수와 `users.tsv` 내용이 바뀌지 않는다. 수정 전 `http.c`로 돌리면 이 테스트가 실패한다. 재실행: Windows 단위 26,052개·CLI 46개, Docker `make test`·`make sanitize`·`make tsan` 서버 16개 실패 0, 검사 도구 보고 없음.

검증 중 고친 결함 2개: JSON 버퍼의 첫 할당이 상한보다 커질 수 있었다(단위 테스트가 잡음). 길이 0 쓰기에서 `memcpy`에 NULL이 넘어갔다(UBSan이 잡음).

spec 대응:

| ID | 테스트 |
|---|---|
| S2 | 서버 테스트 16개: `health_and_empty_body`, `insert_select_explain_match_cli`(JSON 결과 전체 비교, 같은 디렉터리를 CLI로 읽기), `query_string_ignored_and_routes`(404·405·`Allow`), `syntax_error_executes_nothing`(선파싱, NUL), `runtime_error_keeps_earlier_results`, `json_escaping_and_invalid_utf8`, `row_budget_counts_whole_request`(SELECT 여러 개로 우회 불가, 넘긴 SELECT는 통째로 버림), `byte_budget_and_insert_reservation`(응답 ≤ 4096바이트, 응답에 보인 INSERT 수 = 저장된 행 수), `http_level_errors`(413·431·400 형식·chunked·Content-Length 충돌, HTTP/1.0·LF만), `bad_bytes_in_target_or_headers_execute_nothing`(경로·헤더의 NUL·제어 문자 → 400, 파일 변화 없음), `expect_100_continue`, `client_disconnect_does_not_stop_server`, `shutdown_and_restart_keep_data`(SIGTERM → 0, 다음 id 이어짐), `usage_and_start_errors`, `index_failure_halts_and_restart_recovers`(`stored_id` 64, 503 `HALTED`, 재시작 뒤 64 조회), `write_failure_halts_and_restart_is_refused`(`stored_id` 없음, 재시작 거부). 단위: `test_json_escape_and_utf8`, `test_json_budget` |

### P3-03 — 스레드 풀과 작업 큐

**흐름:** accept 스레드는 연결을 큐에 넣기만 한다. 워커 N개가 큐에서 꺼내 P3-02와 같은 처리를 한다. 엔진은 아직 전역 잠금이라 실행은 한 번에 하나씩이고, 네트워크 입출력만 동시에 일어난다.

**확인 시나리오:**
- 느린 클라이언트(`nc`로 연결만 열어 두기) 하나가 있어도 다른 `curl` 요청이 바로 처리된다.
- `--threads 1 --queue 1`로 띄우고 동시에 여러 요청을 보내면 일부가 즉시 503 `BUSY`를 받는다.
- 긴 요청 도중 Ctrl+C를 누르면 처리 중이던 요청은 끝나고, 대기 중이던 요청은 503 `SHUTTING_DOWN`을 받고, 서버가 종료 코드 0으로 끝난다.

**파일:** 새 `src/workqueue.{h,c}`. 바뀌는 곳: `src/server.c`, `tests/server_test.py`.

**구현 메모 (2026-11-17):**
- 큐(`workqueue.c`): 고리 버퍼 + mutex + 조건 변수. `wq_try_push`는 기다리지 않고 가득 차면 실패, `wq_pop`은 빌 때 잠든다. `wq_close`는 닫으면서 아무도 꺼내지 않은 fd를 한꺼번에 돌려준다(종료 때 503용). 이후 `wq_pop`은 바로 -1. pthread만 쓰므로 Windows 단위 테스트에서도 검사한다.
- 워커는 SIGINT·SIGTERM을 막은 채 만든다. 신호는 accept 스레드(main)만 받는다.
- 종료 유예: main이 20 ms 간격으로 살아 있는 워커 수를 본다. 유예가 지나면 `g_force`(원자 변수)를 세우고 처리 중인 연결을 `shutdown`한다. 워커는 문장마다 실행 직전에 `g_force`를 보고 멈춘다. 워커는 연결을 `close`하기 전에 자기 칸을 비우고(같은 mutex), main은 그 mutex 안에서만 `shutdown`하므로 이미 닫혀 번호가 재사용된 fd를 건드리지 않는다.
- 503 고정 응답은 accept 스레드에서 비차단으로 보낸다. 이미 도착한 바이트(최대 64 KiB)만 비차단으로 버리고 닫는다(연결 재설정으로 503이 사라지는 것을 줄이려고. spec 표에 반영).
- 옵션 범위: `--threads` 1~256, `--queue` 1~4096(spec 반영). listen 대기열은 64에서 128로 늘렸다.
- 테스트 전용 빌드에만 있는 조절 장치 2개를 더했다: `MINIDB_TEST_GRACE_MS`(유예 시간 단축), `MINIDB_TEST_STMT_DELAY_MS`(문장마다 잠금 밖에서 잠깐 쉼). 배포용 `minidb-server`에는 없다.

**P3-03 검증 기록(2026-11-17)**

| 명령 | 환경 | 결과 |
|---|---|---|
| `mingw32-make`, `mingw32-make test` | Windows, TDM-GCC 10.3 | 경고 0, 단위 26,071개 검사 실패 0(작업 큐 포함), CLI 46개 실패 0 |
| `make`, `make test` | Docker `gcc:13` | 경고 0, 단위·CLI·서버 23개 실패 0. 서버 테스트 5회 연속 실행 모두 통과(시간에 기대는 테스트의 흔들림 확인) |
| `make sanitize` | Docker `gcc:13`, ASan/UBSan/LSan | 단위·CLI·서버 실패 0, 보고 없음 |
| `docker run --security-opt seccomp=unconfined ... make tsan` | Docker `gcc:13`, TSan | 단위(작업 큐 생산자 4·소비자 4 포함)·서버 실패 0, 보고 없음 |
| 변이 9개 | Docker `gcc:13` | 8개 잡힘: 워커 1개로, 큐가 차면 503 없이 닫기, 대기 연결에 503 없이 닫기, 문장 경계 검사 제거, 유예 뒤 강제 종료 제거, 수신 전체 기한 60초로, 고리 버퍼 순환 제거(단위), 가득 참 검사 제거(단위). 못 잡은 1개("닫힌 큐에 남은 항목을 꺼냄")는 `wq_close`가 닫으면서 항목을 모두 가져가므로 동작이 같은 변이다 |
| walkthrough 6장 실습 1~3 | Windows PowerShell → Docker 서버 | 느린 요청이 있어도 `/health` 239 ms(P3-02는 3,378 ms). `--threads 1 --queue 1`에서 503 `BUSY` 237 ms, 대기 요청은 408 뒤에 200. SIGINT 때 대기 요청 503 `SHUTTING_DOWN`, 처리 중 요청은 408까지 처리하고 종료 |

spec 대응:

| ID | 테스트 |
|---|---|
| S3 | 서버 테스트 7개 추가(총 23개): `slow_client_does_not_block_others`(느린 연결이 있어도 다른 요청 2초 안), `busy_is_immediate_when_queue_full`(`--threads 1 --queue 1`, 503 `BUSY` 2초 안·200바이트 미만, 큐에서 기다린 요청은 처리됨), `concurrent_inserts_over_http`(클라이언트 16개 × 15번 INSERT+조회: id 1..240 중복·빠짐 없음, 성공한 INSERT는 바로 조회됨, CLI로 같은 행 수), `receive_deadlines`(헤더 멈춤 → 5초 408, 1초에 1바이트 → 10초 408, 그 사이 다른 요청 2초 안), `shutdown_finishes_running_and_rejects_queued`(처리 중 요청은 200으로 끝까지, 대기 요청 503 `SHUTTING_DOWN`·저장 안 됨, 종료 코드 0), `shutdown_grace_stops_at_statement_boundary`(유예가 지나면 연결을 끊고 문장 1만 저장), `shutdown_grace_closes_slow_connection`(느린 연결의 기한까지 기다리지 않고 종료). `usage_and_start_errors`에 `--threads`·`--queue` 범위 추가. 단위: `test_workqueue_basic`, `test_workqueue_threads` |

### P3-04 — 읽기 병렬화

**흐름:** SELECT·EXPLAIN은 공유 잠금, INSERT는 배타 잠금을 잡는다. 조회는 `FILE*` 대신 `pread`로 읽고, 시작 시점의 `file_size`까지만 읽는다.

**확인 시나리오:**
- 선형 탐색 요청 여러 개를 동시에 보낸다. 전역 잠금 빌드보다 전체 시간이 줄어드는지 본다(코어 수에 따라 다르다).
- 선형 탐색이 도는 동안 INSERT를 보내 얼마나 기다리는지 본다(writer 기아 관찰).
- 동시 정확성 테스트와 TSan이 그대로 통과한다.

**구현 메모 (2026-11-17):**
- `db.c`: `pthread_mutex` → `pthread_rwlock` 하나. INSERT만 배타 잠금, SELECT·EXPLAIN·`db_state`·`db_stats`는 공유 잠금. 중단 상태로 바꾸는 쓰기는 INSERT 실패 때만 있으므로 항상 배타 잠금 안이다.
- `storage.c` 조회 3곳(`table_scan`, `table_find_by_id`, `table_find_id_range`)은 공유 `FILE*`를 쓰지 않는다. `read_at`(POSIX `pread`, Windows `ReadFile`+`OVERLAPPED`)으로 위치를 지정해 읽고, 읽기 버퍼는 호출마다 따로 둔다(id 조회는 스택 320바이트, 선형 탐색·범위는 64 KiB를 할당). `FILE*`는 INSERT만 쓴다. Windows CLI도 같은 코드라 동시 정확성 테스트가 Windows에서도 이 경로로 돈다.
- `file_size`: 시작 시 검증하며 센 바이트 수로 정하고, INSERT가 줄을 끝까지 쓰고 `fflush`한 뒤에만 늘린다(배타 잠금 안). 조회는 시작 시점의 `file_size`까지만 읽는다.
- **writer 기아 → 쓰기 우선으로 바꿈(설계에 적어 둔 대응).** 첫 측정에서 glibc 기본(읽기 우선) rwlock은 선형 탐색 4개가 계속 들어오는 동안 INSERT가 20초 넘게 응답을 받지 못했다. 공유 잠금이 한 번도 비지 않았기 때문이다. glibc에서는 `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`로 잠금을 만든다. 바꾼 뒤 같은 부하에서 INSERT 중앙값 104 ms, 최대 159 ms. 이 속성은 같은 스레드가 읽기 잠금을 겹쳐 잡으면 교착하므로, 엔진은 잠금을 한 번만 잡고 콜백 안에서 `db_*`를 부르지 않는다(지금 코드가 그렇다). glibc가 아닌 곳(Windows의 단위 테스트 등)은 기본 잠금이다.
- 비교용 빌드 `-DMINIDB_GLOBAL_LOCK`: 읽기도 배타 잠금을 잡는다(P3-03까지와 같은 동작). 측정(`make read-bench`)에만 쓴다.

**P3-04 측정 (`make read-bench`, 2026-11-17, Docker `gcc:13`, CPU 8개, 100만 행, 클라이언트·워커 4개, 3회 중앙값):**

| 항목 | 전역 잠금 (P3-03까지) | 읽기·쓰기 잠금 (P3-04) |
|---|---|---|
| 선형 탐색 1개 응답 | 199 ms | 190 ms |
| 선형 탐색 4개 동시, 전체 시간 | 813 ms (1개의 4.1배) | **313 ms** (1.6배) |
| INSERT 응답 (부하 없음) | 1.30 ms | 1.06 ms |
| INSERT 응답 (선형 탐색 4개가 계속 도는 중), 중앙값 / 최대 | 550 ms / 671 ms | **104 ms / 159 ms** |
| (참고) 읽기 우선 기본값일 때 같은 INSERT | — | 20초 넘게 응답 없음 |

- 전역 잠금에서는 4개가 한 줄로 서서 1개의 약 4배가 걸린다. 공유 잠금에서는 동시에 읽어 1.6배다. 1배가 아닌 것은 디스크·메모리 대역폭과 JSON 만들기를 나눠 쓰기 때문이다. 숫자는 CPU 수와 디스크(페이지 캐시) 상태에 따라 다르다.
- 쓰기 우선이라 INSERT는 이미 돌고 있는 탐색들이 끝날 때까지만 기다린다(탐색 1개 시간 안쪽). 전역 잠금보다 짧은 것은 그 뒤에 온 탐색이 INSERT를 앞지르지 못하기 때문이다.

**P3-04 검증 기록(2026-11-17)**

| 명령 | 환경 | 결과 |
|---|---|---|
| `mingw32-make`, `mingw32-make test` | Windows, TDM-GCC 10.3 | 경고 0, 단위 29,119개 검사 실패 0(쓰기 우선 테스트는 glibc 전용이라 제외), CLI 46개 실패 0 |
| `make`, `make test` | Docker `gcc:13` | 경고 0, 단위 29,127개·CLI 46개·서버 23개 실패 0. 단위 테스트 5회 연속 통과 |
| `make sanitize` | Docker `gcc:13`, ASan/UBSan/LSan | 단위·CLI·서버 실패 0, 보고 없음 |
| `docker run --security-opt seccomp=unconfined ... make tsan` | Docker `gcc:13`, TSan | 단위·서버 실패 0, 보고 없음(읽기가 실제로 동시에 도는 상태). 단위 TSan 2회 더 반복 통과 |
| 변이 8개 | Docker `gcc:13` | 모두 잡힘: 읽기도 배타 잠금(`MINIDB_GLOBAL_LOCK`), INSERT도 공유 잠금, id 조회 버퍼를 `static`으로 공유(일반·TSan 빌드 둘 다), INSERT 뒤 `file_size` 안 늘림, 선형 탐색이 `file_size` 대신 파일 끝까지 읽음, 쓰기 우선 → 읽기 우선 |
| `make race-demo` | Docker `gcc:13` | 위 "P3-04 뒤 다시 관찰" |

spec 대응:

| ID | 테스트 |
|---|---|
| S4 | 단위 5개 추가: `test_storage_parallel_reads_without_lock`(스레드 8개가 잠금 없이 같은 `Table`을 id·범위·전체 조회, 결과 전부 정확 → 조회 경로에 공유 상태 없음), `test_storage_scan_stops_at_file_size`(파일 끝에 덜 쓰인 줄이 붙어도 조회는 `file_size`까지만), `test_db_readers_run_together`(SELECT 둘이 동시에 잠금 안에 있음), `test_db_writer_waits_for_reader`(SELECT가 잠금 안에 있는 동안 INSERT는 끝나지 않음), `test_db_writer_not_starved`(glibc: 기다리는 INSERT 뒤에 온 SELECT는 INSERT보다 먼저 들어가지 못함). 기존 `test_db_concurrent_correctness`(C1~C4)는 이제 읽기가 실제로 동시에 도는 상태에서 통과. 처리량 비교: `make read-bench`(위 표) |

**파일:** 바뀌는 곳: `src/storage.c`(읽기 경로 `pread`, `file_size`), `src/db.c`(rwlock), 측정 스크립트.

### P3-05 — 부하 측정과 마무리

**흐름:** `tools/load.py`로 클라이언트 수·워커 수·요청 종류(id 조회 / 선형 탐색 / INSERT 섞기)를 바꿔 처리량과 응답 시간 분포를 잰다.

**확인 시나리오:** `docs/phase3/benchmark.md` 표에서 워커 수, 잠금 방식, 요청 종류별 차이를 읽는다. Phase 2의 "실행 한 번 기준 2배 미만" 결과와 서버 구조의 차이를 비교한다.

**파일:** 새 `tools/load.py`, `docs/phase3/benchmark.md`. 바뀌는 곳: README, docs/phase3/*.

**구현 메모 (2026-11-17):**
- 부하 생성기를 C로 따로 만들었다(`tools/loadgen.c`, 클라이언트마다 스레드 하나, 닫힌 고리). 처음에는 `load.py`가 파이썬 프로세스로 요청을 보냈는데, 클라이언트가 요청마다 쓰는 CPU가 커서 워커를 늘려도 처리량이 그대로였다(서버가 아니라 클라이언트를 잰 셈). `load.py`는 서버 띄우기·설정 바꾸기·표 만들기만 한다. `make load-bench`.
- 측정 중 발견한 환경 병목 두 가지와 대응(benchmark.md 5):
  - 데이터·서버 로그를 Windows 공유 폴더에 두면 요청마다 로그 쓰기(약 1 ms, 한 번에 하나)에서 모든 워커가 줄을 서서 처리량이 약 1,000건/초에서 멈췄다. 측정 도구(`load.py`, `read_parallel.py`)는 이제 컨테이너 안 임시 폴더를 쓴다. 그래서 P3-04 때 잰 `read-bench` 값도 다시 쟀다(813 → 313 ms였던 것이 임시 폴더에서 493 → 100 ms). walkthrough 7장 표를 새 값으로 바꿨다.
  - 서버가 먼저 닫아 TIME_WAIT 연결이 서버 쪽에 60초씩 쌓이고, 상한(32,768)에 닿으면 처리량이 절반 가까이 떨어졌다. 부하 생성기는 응답을 끝까지 받은 뒤 RST(`SO_LINGER` 0)로 닫아 쌓이지 않게 한다. 실제 클라이언트와 다른 점은 benchmark.md에 적었다.
- 설정마다 2초씩 3회 재고 처리량이 가운데인 회차를 쓴다(한 번만 재면 같은 설정이 2배 넘게 흔들렸다).
- 서버 코드는 바꾸지 않았다. 밀린 문서 수정 2개: walkthrough 6장 실습 2에 UTF-8 출력 설정 줄과 "시작 줄에서 `워커 1개, 큐 1` 확인" 안내.

**P3-05 결과 요약 (benchmark.md, Docker `gcc:13`, CPU 8개, 100만 행):**

| 질문 | 값 |
|---|---|
| id 하나 조회: CLI 한 번 실행 vs 떠 있는 서버 | 189 ms vs 0.23 ms (Phase 2의 "실행 한 번 기준 2배 미만"은 시작 비용 때문이었고, 서버는 그 비용을 한 번만 낸다) |
| id 조회 처리량: 워커 1 / 2 / 4 / 8개 (클라이언트 16개) | 5,966 / 11,956 / 20,311 / 22,733건/초 |
| 선형 탐색(클라이언트 4개): 전역 잠금 vs 읽기·쓰기 잠금 | 15 vs 46건/초, p99 2,284 vs 106 ms |
| id 조회(클라이언트 8개): 전역 잠금 vs 읽기·쓰기 잠금 | 17,070 vs 21,286건/초 (잠금 안 시간이 짧아 차이가 작다) |
| 몰릴 때 503: 워커 1개·클라이언트 16개, 큐 1칸 vs 64칸 | 2초 동안 33,371번 vs 0번 (대신 p50 0.80 → 2.12 ms) |

**P3-05 검증 기록(2026-11-17)**

| 명령 | 환경 | 결과 |
|---|---|---|
| `mingw32-make`, `mingw32-make test` | Windows, TDM-GCC 10.3 | 경고 0, 단위 29,119개 검사 실패 0, CLI 46개 실패 0 |
| `make clean && make test` | Docker `gcc:13` | 경고 0, 단위 29,127개·CLI 46개·서버 23개 실패 0 |
| `make sanitize` | Docker `gcc:13`, ASan/UBSan/LSan | 단위·CLI·서버 실패 0, 보고 없음 |
| `docker run --security-opt seccomp=unconfined ... make tsan` | Docker `gcc:13`, TSan | 단위·서버 실패 0, 보고 없음 |
| `make bench BENCH_ROWS=100000` | Docker `gcc:13` | Phase 2 측정 도구 정상 동작(INDEX LOOKUP·RANGE SCAN·FULL SCAN 표 출력) |
| `make load-bench`, `make read-bench` (`READ_CLIENTS=4`, `16`) | Docker `gcc:13` | benchmark.md와 walkthrough 7·8장의 표 |

spec 대응:

| ID | 확인 |
|---|---|
| S5 | 부하 측정: benchmark.md 1~6. Phase 1·2 회귀: CLI 테스트 46개가 A1~A8(`a1_`~`a7_` 테스트와 sanitize)과 B3~B7(`p2_` 테스트)을, 단위 테스트가 B1·B2(B+ 트리)를 덮고 Windows·Linux·sanitize에서 모두 통과(B8). ASan·TSan 통과. 문서: README, benchmark.md, walkthrough 8장 |

## 공통 규칙 (Phase 1·2에서 이어짐)

- 경고 0으로 빌드한다(`-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes`).
- 테스트를 추가하면 변이 테스트로 확인한다. 정상 빌드에서 한 줄을 바꿨을 때 테스트가 실패해야 한다. 잠금 없는 빌드의 결과는 판정에 쓰지 않는다.
- Windows `mingw32-make test`가 계속 통과해야 한다.
