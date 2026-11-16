# Phase 3 구현 순서

## 현재 상태

**P3-01 완료(2026-11-16).** 다음은 P3-02(단일 스레드 HTTP 서버). P3-02가 끝나면 `walkthrough.md`로 요청·응답 실습을 시작할 수 있다.

진행 방식(Phase 2 DB-P2-D10 유지): 한 단위를 구현한다 → 테스트와 ASan·TSan 검사로 검증한다 → 통과하면 바로 커밋하고 다음 단위로 간다. 사용자 동작이나 공개 인터페이스를 바꾸는 결정은 구현 전에 짧게 제안한다.

| ID | 기능 | 완료 기준 | 상태 |
|---|---|---|---|
| P3-00 | 구현 전 결정 | design.md DB-P3-D01~D11 확정 | 완료(2026-11-16) |
| P3-01 | 엔진 진입점과 전역 잠금 | S1, S1x | 완료 |
| P3-02 | 단일 스레드 HTTP 서버 | S2, 실행 예시와 요청 경로 지도 | 대기 |
| P3-03 | 스레드 풀과 작업 큐 | S3 | 대기 |
| P3-04 | 읽기 병렬화 | S4 | 대기 |
| P3-05 | 부하 측정과 마무리 | S5 | 대기 |

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

### P3-02 — 단일 스레드 HTTP 서버

**흐름:** 연결을 받는다 → 요청을 읽는다 → 문장 전체를 파싱한다 → 문장마다 `db_exec`(JSON Sink) → 응답을 보낸다 → 연결을 닫는다. 한 번에 연결 하나씩 처리한다.

**확인 시나리오** (`walkthrough.md`에 명령과 기대 응답을 넣는다):
- `curl`로 INSERT → id 조회 → 범위 조회 → EXPLAIN을 보낸다. CLI 결과와 같은 데이터가 JSON으로 온다.
- 구문 오류가 섞인 요청을 보내면 400 `SQL_SYNTAX`이 오고, 아무것도 저장되지 않았음을 조회로 확인한다.
- 실행 단계 오류에서는 앞 INSERT만 저장되고 `statement` 번호가 온다.
- `--max-rows 3`으로 띄우고 SELECT를 여러 개 보내면 `RESULT_TOO_LARGE`가 오고, 앞 결과만 남는다.
- 서버를 끄고 다시 띄워도 데이터가 그대로다.

**파일:** 새 `src/server.c`, `src/http.{h,c}`, `src/json.{h,c}`, `tests/server_test.py`, `docs/phase3/walkthrough.md`. 바뀌는 곳: `Makefile`(`minidb-server`, POSIX에서만).

### P3-03 — 스레드 풀과 작업 큐

**흐름:** accept 스레드는 연결을 큐에 넣기만 한다. 워커 N개가 큐에서 꺼내 P3-02와 같은 처리를 한다. 엔진은 아직 전역 잠금이라 실행은 한 번에 하나씩이고, 네트워크 입출력만 동시에 일어난다.

**확인 시나리오:**
- 느린 클라이언트(`nc`로 연결만 열어 두기) 하나가 있어도 다른 `curl` 요청이 바로 처리된다.
- `--threads 1 --queue 1`로 띄우고 동시에 여러 요청을 보내면 일부가 즉시 503 `BUSY`를 받는다.
- 긴 요청 도중 Ctrl+C를 누르면 처리 중이던 요청은 끝나고, 대기 중이던 요청은 503 `SHUTTING_DOWN`을 받고, 서버가 종료 코드 0으로 끝난다.

**파일:** 새 `src/workqueue.{h,c}`. 바뀌는 곳: `src/server.c`, `tests/server_test.py`.

### P3-04 — 읽기 병렬화

**흐름:** SELECT·EXPLAIN은 공유 잠금, INSERT는 배타 잠금을 잡는다. 조회는 `FILE*` 대신 `pread`로 읽고, 시작 시점의 `file_size`까지만 읽는다.

**확인 시나리오:**
- 선형 탐색 요청 여러 개를 동시에 보낸다. 전역 잠금 빌드보다 전체 시간이 줄어드는지 본다(코어 수에 따라 다르다).
- 선형 탐색이 도는 동안 INSERT를 보내 얼마나 기다리는지 본다(writer 기아 관찰).
- 동시 정확성 테스트와 TSan이 그대로 통과한다.

**파일:** 바뀌는 곳: `src/storage.c`(읽기 경로 `pread`, `file_size`), `src/db.c`(rwlock), 측정 스크립트.

### P3-05 — 부하 측정과 마무리

**흐름:** `tools/load.py`로 클라이언트 수·워커 수·요청 종류(id 조회 / 선형 탐색 / INSERT 섞기)를 바꿔 처리량과 응답 시간 분포를 잰다.

**확인 시나리오:** `docs/phase3/benchmark.md` 표에서 워커 수, 잠금 방식, 요청 종류별 차이를 읽는다. Phase 2의 "실행 한 번 기준 2배 미만" 결과와 서버 구조의 차이를 비교한다.

**파일:** 새 `tools/load.py`, `docs/phase3/benchmark.md`. 바뀌는 곳: README, docs/phase3/*.

## 공통 규칙 (Phase 1·2에서 이어짐)

- 경고 0으로 빌드한다(`-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes`).
- 테스트를 추가하면 변이 테스트로 확인한다. 정상 빌드에서 한 줄을 바꿨을 때 테스트가 실패해야 한다. 잠금 없는 빌드의 결과는 판정에 쓰지 않는다.
- Windows `mingw32-make test`가 계속 통과해야 한다.
