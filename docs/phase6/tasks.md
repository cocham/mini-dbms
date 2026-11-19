# Phase 6 구현 순서 — age 조건 페이지 조회 (선형 탐색 기준선 → age 보조 인덱스)

## 현재 상태

**P6-01~P6-05 구현·자체 검증 완료(2026-11-18), 사용자 검토·커밋 대기.** P6-00 확정(2026-11-18, 사용자: D03~D12, D14~D17 추천안). 브랜치 `phase6-age-filter`(`phase5-pagination` `0a5522e`에서 시작). Phase 5 PR이 아직 main에 merge되지 않아 그 위에서 시작했다. Phase 5 PR을 먼저 merge해야 이 브랜치의 PR이 Phase 6 변경만 보여 준다.

최종 목표(사용자, 2026-11-18): age 조건 페이지 조회를 보조 인덱스로 개선하고 같은 요청의 개선 전후를 비교한다. 정리 문서는 [README.md](README.md).

| ID | 기능 | 완료 기준 | 상태 |
|---|---|---|---|
| P6-00 | 구현 전 결정 | design.md DB-P6-D01~D17 확정 | 완료(2026-11-18) |
| P6-01 | 엔진 조건 페이지(선형 탐색) + 통계 | F1, F2, F5(단위) | 구현·테스트 완료 |
| P6-02 | `GET /users?age=` + 계측 헤더 | F3, F4, F5(서버) | 구현·테스트 완료 |
| P6-03 | 대표 실험 도구(선형 탐색 기준선) | F6 | 완료 |
| P6-04 | age 보조 인덱스(엔진) + 유지 + 두 경로 동등성 | F8, F9, F5(인덱스) | 구현·테스트 완료 |
| P6-05 | 서버 경로 선택·헤더, 전후 비교, README 결과 정리 | F4(인덱스), F10 | 완료. 대표 실험 [benchmark.md](benchmark.md) (사용자 요청으로 직접 실행, 2026-11-18) |
| — | 회귀 | F7 | 통과 |

**진행 방식(사용자 2026-11-18 수정):** P6-01~P6-05를 순서대로 구현하고 단위마다 그 단위의 테스트를 실행한 뒤 다음 단위로 갔다(매번 확인을 기다리지 않음). 커밋은 사용자가 검토한 뒤 한다. main에는 직접 커밋·푸시하지 않는다. 사용자 학습 문서(README-experiments.md 등)는 커밋에서 뺀다.

## 단위별 흐름과 확인 시나리오

### P6-01 — 엔진 조건 페이지(선형 탐색) + 통계

**흐름:** `table_page(t, after_id, limit, filter, fn, ctx, &st, err)`. 필터가 없으면 Phase 5 경로 그대로(N+1은 키로만)다. 필터가 있으면 B+ 트리로 `after_id + 1`을 찾고, 리프 연결을 따라 키마다 줄을 읽어 age를 비교한다(E++). 맞는 행은 `limit`개까지 `fn`에 넘기고(R++), `limit + 1`번째로 맞는 행을 만나면 `has_more = 1`로 정상 멈춘다. 마지막 반환 행 뒤에 읽은 줄은 T에 센다. `db_page`는 같은 인자를 공유 잠금 안에서 넘긴다.

**확인 시나리오(손으로 계산한 기대값과 비교):**
- id 1~20, age는 짝수 id만 30: `age 30, limit 3` → `[2,4,6]`, E 8(1~8번 줄), T 2(7·8번), `has_more 1`. 이어서 `after 6` → `[8,10,12]`.
- 맞는 행이 정확히 `limit`개 남고 뒤에 안 맞는 꼬리: `has_more 0`, T = 꼬리 길이.
- `limit + 1`번째로 맞는 행이 마지막 줄: `has_more 1`, 다음 요청은 그 한 행.
- 일치 없음: 행 없음, E = 커서 뒤 행 수, T = E.
- 빈 테이블, `after_id ≥ 최대 id`, `INT64_MAX`: E 0.
- 커서가 맞지 않는 행이거나 지워진 행: 그 뒤 첫 행부터.
- id 공백(DELETE 뒤), UPDATE로 age가 바뀐 뒤(다시 쓰기): 바뀐 값 기준으로 같은 규칙.
- 필터 없음: Phase 5 테스트가 인자만 바뀌고 그대로 통과. E = R, T = 0, 테스트 카운터(읽은 줄·본 키)도 Phase 5와 같음.
- 동시성: 조건 순방향 순회 + 쓰기 스레드(INSERT·UPDATE age 변경·DELETE). design.md "테스트"의 (가)(나)(다). TSan.

**파일:** `src/storage.{h,c}`, `src/db.{h,c}`, `src/server.c`(호출 인자만), `tests/test_unit.c`.

### P6-02 — `GET /users?age=` + 계측 헤더

**흐름:** `parse_users_query`가 `age`를 받는다(D03·D04). 서버는 `db_page` 앞뒤로 시간을 재고(D10), 200 응답에 헤더 세 개를 붙이며(D09), 요청 로그에 R·E·T·엔진 시간을 적는다.

**확인 시나리오:**
- spec "입력 검증" 표의 모든 경우가 400 `INVALID_PARAM`이고 엔진을 부르지 않는다.
- 응답 형식과 헤더 값(작은 데이터에서 손으로 계산한 E·T).
- 필터 없는 응답 본문이 Phase 5와 바이트 단위로 같다. 헤더만 더해진다.
- 조건 순방향 조회(F4): 수천 행에서 `age = a`로 끝까지 따라가면 기대 목록과 같다.
- 동시 순회(F5): HTTP로 조건 순회 + 다른 클라이언트들의 쓰기.

**파일:** `src/server.c`, `tests/server_test.py`.

### P6-03 — 대표 실험 도구(선형 탐색 기준선)

**흐름:** 측정 도구(`tools/age_bench.py`, `make age-bench`)가 D11의 데이터(100만 행, age 25이 1,000행)를 만들고, 서버를 기본 설정으로 띄워 `GET /users?age=25&after_id=0&limit=100`을 워밍업 5회 뒤 30회 잰다. 헤더 E·T가 기대값(100,001·1,000)과 다르면 실패로 끝난다. P6-05에서 같은 도구에 경로 비교를 더한다.

**기록:** `docs/phase6/benchmark.md`에 환경·데이터 분포·반복 횟수·커밋·관찰값을 적는다. 해석은 관찰값과 추정을 나눈다.

**파일:** `tools/age_bench.py`, `Makefile`, `docs/phase6/benchmark.md`.

### P6-04 — age 보조 인덱스(엔진)

**흐름:** `Table`에 age별 id 배열(D14)을 둔다. `table_open`의 시작 탐색, `table_append`, `table_rewrite`(새 id 인덱스와 함께 만들어 함께 교체)에서 유지한다(D16). `table_page`는 `PageFilter`의 경로가 인덱스이면 age 배열에서 `after_id`보다 큰 첫 항목을 이진 탐색으로 찾고, 차례로 최대 `limit + 1`개 항목을 본다(K). 앞의 `limit`개는 id 인덱스로 줄 위치를 찾아 읽고 id·age를 재확인한다(E = R). `limit + 1`번째 항목이 있으면 `has_more = 1`이고 그 줄은 읽지 않는다.

**확인 시나리오:**
- 인덱스 일치(F8): INSERT, UPDATE(age 변경 / name만 변경), DELETE, 다시 쓰기 실패 단계별(실패 뒤 옛 인덱스 그대로), INSERT의 인덱스 등록 실패, 재시작 뒤 `table_test_validate_age_index` 통과.
- 두 경로 동등성(F9): 무작위 데이터·무작위 쓰기 뒤 age 0~150 전부 × 여러 커서 × limit 1·3·100에서 두 경로의 행·`has_more`·커서가 같음. 인덱스 경로의 E = R, K = R + (has_more ? 1 : 0), T = 0.
- 인덱스 불일치 감지: 테스트 전용으로 배열에 잘못된 id를 넣으면 `DBE_STORAGE`.
- 동시성: 인덱스 경로로 조건 순방향 순회 + 쓰기 스레드. TSan.

**파일:** `src/storage.{h,c}`, `src/db.{h,c}`, `tests/test_unit.c`.

### P6-05 — 경로 선택, 대표 실험 전후 비교

**흐름:** 서버 옵션 `--age-path scan|index`(D17), 헤더 `X-Age-Index-Keys`·`X-Access-Path`, 시작 줄에 경로 표시. 측정 도구가 같은 데이터·설정으로 scan → index → index → scan 순서로 서버를 띄워 같은 요청을 잰다. 두 경로의 응답 본문이 바이트 단위로 같은지, 헤더 E·T·K가 기대값과 같은지 확인한다.

**확인 시나리오:**
- 서버 테스트: 두 옵션에서 같은 요청의 본문이 같음, 헤더 값, 조건 순방향 순회(F4)를 인덱스 경로로 다시, INSERT·UPDATE·DELETE 뒤와 재시작 뒤 두 경로 결과가 같음.
- 대표 실험(F10): 반환 행 수·내용 동일, E(선형 100,001 / 인덱스 100), K(0 / 101), 클라이언트 응답 시간 중앙값.

**기록:** `docs/phase6/benchmark.md`에 전후 비교, `README.md`에 실제 결과를 정리한다. 관찰값과 추정을 나누고, 쓰기 성능·메모리는 측정하지 않았으면 유지 작업과 한계로만 적는다.

**파일:** `src/server.c`, `tests/server_test.py`, `tools/age_bench.py`, `docs/phase6/benchmark.md`, `docs/phase6/README.md`.

## 구현 메모 (2026-11-18)

- **엔진 인터페이스(D06):** `table_page(t, after_id, limit, filter, fn, ctx, &st, err)`, `db_page`도 같은 인자. `PageFilter{has_age, age, path}`, `PageStats{has_more, rows_returned, rows_examined, examined_after_last, index_keys, used}`. 경로는 `PAGE_PATH_AUTO`(= 인덱스)·`SCAN`·`INDEX`, 실제 경로는 `PAGE_USED_ID`·`SCAN`·`AGE_INDEX`.
- **선형 탐색(D07):** Phase 5의 `range_visit`에 age 비교를 더했다. 조건이 없으면 N+1은 키로만(줄을 읽지 않음), 조건이 있으면 `limit + 1`번째로 맞는 줄을 찾을 때까지 읽는다. T는 마지막 반환 행 뒤에 읽은 줄 수로 센다(R = 0이면 T = E).
- **age 인덱스(D14~D16):** `struct AgeIndex { IdList by[151]; }`, `IdList`는 id 오름차순 배열. 시작 탐색(`table_open`)에서 id 인덱스와 함께 만들고, INSERT는 id 인덱스 등록 뒤 배열 끝에 붙이며, 다시 쓰기는 새 id 인덱스와 같은 반복에서 새로 만들어 바꿔치기 성공 뒤 함께 교체한다. 실패하면 새것을 버린다.
- **인덱스 경로(D15):** 이진 탐색으로 `after_id`보다 큰 첫 항목을 찾고, 항목마다 K++. `limit`개를 넘긴 뒤 다음 항목이 있으면 `has_more = 1`이고 그 줄은 읽지 않는다. 항목마다 `bpt_search`로 줄 위치를 찾아 한 줄을 읽고 id·age를 다시 확인한다(다르면 `DBE_STORAGE`). E = R, T = 0.
- **INSERT의 age 인덱스 등록 실패:** id 인덱스 실패와 같은 `DBE_INDEX_FAILED` → 엔진 중단(`DB_HALTED_INDEX`), 재시작하면 파일에서 재구성.
- **서버:** `age` 파라미터(D03·D04), 계측 헤더 5개(`X-Rows-Examined`·`X-Rows-Examined-After-Last`·`X-Age-Index-Keys`·`X-Access-Path`·`X-Engine-Us`), 요청 로그 요약, `--age-path scan|index`(기본 index), 시작 줄 끝에 `age 경로 index|scan`. 헤더는 P6-02에서 다섯 개를 한 번에 넣었다(D09의 세 개 + D16의 두 개).
- **측정 도구:** `tools/age_bench.py`, `make age-bench`. 데이터·기대값을 같은 규칙에서 계산하고, 본문 바이트 비교·경로·E·T·K를 자동 검사한다. 순서 항목 `default`는 `--age-path` 없이 띄운다.
- **테스트 도우미의 경쟁(TSan으로 발견, 테스트 코드만):** P6-01 단계에서 `run_page`가 스레드 안에서 `CHECK`(전역 카운터)를 불러 TSan이 경쟁을 보고했다. 도우미는 `CHECK` 대신 반환값 -9로 알리게 고쳤다. 엔진 코드의 경쟁은 아니다.

## 검증 기록 (2026-11-18)

| 단계 | Windows (TDM-GCC 10.3) | Docker `gcc:13` (컨테이너 안 복사본) |
|---|---|---|
| P6-01 | 단위 44,373·CLI 56 실패 0 | 단위·CLI 56·서버 34·부하 생성기 9 실패 0, sanitize 보고 없음, TSan 보고 없음(도우미 수정 뒤) |
| P6-02 | (서버는 POSIX 전용) | 단위·CLI 56·서버 39·부하 생성기 9 실패 0, sanitize 보고 없음 |
| P6-03 | | `make age-bench AGE_ROWS=20000 AGE_REPEAT=5 AGE_ORDER=default` 통과(선형 탐색 기준선, 본문·E·T 기대값 일치) |
| P6-04 | 단위 48,847·CLI 56 실패 0 | (P6-05와 함께) |
| P6-05 | 단위·CLI 56 실패 0 | 단위 48,844·CLI 56·서버 43·부하 생성기 9 실패 0, sanitize·TSan 보고 없음. `make age-bench AGE_REPEAT=5` 100만 행 통과(도구 확인용, 발표 수치 아님) |

단위 테스트 검사 수는 실행마다 조금 다르다(동시 테스트의 반복 수가 스케줄에 따라 달라짐).

**변이 검사(18개, 모두 잡힘):** 선형 탐색 T를 세지 않음 · 조건이 있어도 N+1을 키로만 판단 · 맞지 않는 행 반환 · 반환 뒤 T를 0으로 되돌리지 않음 · 인덱스 경로가 N+1 행까지 반환 · 인덱스 경로의 age 재확인 생략 · INSERT가 age 인덱스에 등록하지 않음 · 다시 쓰기 뒤 옛 age 인덱스 유지 · 다시 쓰기가 바뀌기 전 age로 등록 · 커서 이진 탐색이 커서 행 포함 · AUTO가 선형 탐색 · 열 때 age 인덱스를 만들지 않음 · `db_page`가 필터를 버림 · 서버가 age 151 허용 · 중복 `age` 허용 · 서버 기본 경로가 scan · 헤더 T 자리에 E · 서버가 `--age-path`를 무시.

## 자체 검증(단위마다)

- Windows: `mingw32-make test`(단위·CLI).
- Docker `gcc:13`(컨테이너 안 복사본에서): `make test`(단위·CLI·서버·부하 생성기), `make sanitize`, `make tsan`.
- 변이 검사: 결함을 하나씩 심어 그 단위의 테스트가 잡는지 확인한다.
