# 미니 DBMS

C로 SQL 파일을 실행하고 데이터를 파일에 저장하는 CLI(Phase 1)에, id B+ 트리 인덱스(Phase 2)를 붙였다.

## Phase 3 — API 서버 (구현 완료)

현재 상태: **P3-01~P3-05 구현 완료.** `POST /query`로 SQL을 보내면 JSON으로 답하는 서버(`build/minidb-server`, POSIX 전용)가 워커 여러 개로 연결을 동시에 처리한다. 큐가 가득 차면 503 `BUSY`로 바로 거절한다. 엔진은 읽기·쓰기 잠금이라 SELECT끼리는 동시에 실행되고 INSERT는 혼자 실행된다(쓰기 우선).

측정([benchmark.md](docs/phase3/benchmark.md), 100만 행, Docker CPU 8개, 같은 환경 안의 상대 비교): id 하나 조회가 CLI 한 번 실행 346 ms → 떠 있는 서버 0.18 ms. id 조회 처리량 워커 1개 7,434 → 8개 30,109건/초. 선형 탐색 4개 동시 전역 잠금 304 ms → 읽기·쓰기 잠금 88 ms.

1. [실습](docs/phase3/walkthrough.md): 서버 띄우기 → `curl`로 요청·응답 → 요청 하나가 엔진까지 가는 지도 → (6장) 스레드 풀 → (7장) 읽기 병렬화 → (8장) 부하 측정. **여기서 시작**
2. [개발 명세](docs/phase3/spec.md): API 계약, 오류 코드, 크기 제한, 저장 후 실패, 동시성 계약
3. [설계와 결정](docs/phase3/design.md): 엔진 연결, 지금 코드의 동시성 문제, 잠금 단계, 서버 구조
4. [작업 순서](docs/phase3/tasks.md): P3-01~P3-05, 단위별 흐름·확인 시나리오·검증 기록
5. [부하 측정](docs/phase3/benchmark.md): 워커 수, 요청 종류 × 잠금 방식, 큐 크기, 측정 중 발견한 환경 병목

```bash
# Windows에서 Docker로 서버 띄우기 (자세한 것은 walkthrough)
docker run --rm -it -p 127.0.0.1:8080:8080 -v "$PWD:/src" -w /src gcc:13 bash -c "make build/minidb-server && mkdir -p data/p3 && build/minidb-server --host 0.0.0.0 --port 8080 --data-dir data/p3"
curl.exe -s http://localhost:8080/query --data-binary "SELECT * FROM users WHERE id = 1;"

make test                      # Windows: 단위(동시 정확성 포함)·CLI / Linux: 여기에 서버 테스트까지
docker run --rm --security-opt seccomp=unconfined -v "$PWD:/src" -w /src gcc:13 make tsan       # 데이터 경쟁 검사
docker run --rm --security-opt seccomp=unconfined -v "$PWD:/src" -w /src gcc:13 make race-demo  # 잠금 없는 엔진 관찰 실험
docker run --rm -v "$PWD:/src" -w /src gcc:13 make read-bench   # 전역 잠금 vs 읽기·쓰기 잠금 측정 (약 1분)
docker run --rm -v "$PWD:/src" -w /src gcc:13 make load-bench   # 부하 측정 전체 (약 2분, build/load_result.md)
```

## Phase 2 — B+ 트리 인덱스 (완료)

현재 상태: **P2-01~P2-05 완료.** `WHERE id = ?`는 B+ 트리에서 바이트 위치를 찾아 그 한 줄만 읽고, `WHERE id BETWEEN a AND b`·`id < v` 등은 리프 연결을 따라 범위 안의 줄만 읽고, `EXPLAIN SELECT ...`는 조회 없이 실행 계획(INDEX LOOKUP / INDEX RANGE SCAN / FULL SCAN과 계획 시점 통계)을 보여 준다.

1. [탑다운 학습 가이드](docs/phase2/learning.md): 결과물 → 전체 지도 → 기능별 실습. **여기서 시작**
2. [개발 명세](docs/phase2/spec.md): 최종 결과, 데이터 흐름, Phase 1 대비 변경점
3. [설계와 결정](docs/phase2/design.md): 오프셋 인덱스, 트리 구조, 일관성, 시작 비용
4. [작업 목록](docs/phase2/tasks.md): P2-01~P2-05와 검증 기록
6. [성능 측정](docs/phase2/benchmark.md): 100만 건에서 id 조회 0.02 ms vs age 선형 탐색 0.7~0.8 s, 시작 비용 0.9 s
5. [구현설명서](docs/phase2/implementation.md): id 조회·EXPLAIN·범위 조회를 코드로 따라가기

```text
make                                   # build/minidb, build/bpt_demo (macOS·Linux)
make test                              # 단위 + CLI 통합 테스트
mkdir -p data && build/minidb --data-dir data/p2 examples/phase2_id_lookup.sql
build/minidb --data-dir data/p2e examples/phase2_explain.sql   # EXPLAIN 실행 계획
build/minidb --data-dir data/p2r examples/phase2_range.sql     # id 범위 조회
make bench                             # 100만 건 생성·삽입 후 인덱스 vs 선형 탐색 측정 (약 1분)
build/bpt_demo 3 7 4                   # 차수 3 트리에 1~7을 넣은 모양과 id 4 탐색 경로
docker run --rm -v "$PWD:/src" -w /src gcc:13 make sanitize   # ASan/UBSan/LSan
```

## Phase 1 — SQL 처리기 (완료)

### 읽는 순서

1. [개발 명세](docs/spec.md): 무엇을 만들고 어떻게 확인하는가
2. [설계와 결정](docs/design.md): 처리 흐름, 저장 방식, 구현 전 결정
3. [작업 목록](docs/tasks.md): 구현 단위와 검증 기준
4. [구현설명서](docs/implementation.md): 전체 흐름에서 INSERT·SELECT 코드 따라가기

현재 상태: **Phase 1 구현·테스트 완료(DB-P1-01, DB-P1-C01 SQL 주석, DB-P1-02 조건 조회), DB-P1-02 Codex 검증 대기.**

지원 SQL: `INSERT INTO users (name, age) VALUES (...)`, `SELECT * FROM users [WHERE id = 정수 | WHERE age = 정수]`, `--`·`/* */` 주석. 조건 조회는 선형 탐색이다.

### 빌드와 실행 (Windows, TDM-GCC)

```text
mingw32-make                 # build/minidb.exe
mingw32-make test            # 단위 테스트 + CLI 통합 테스트
build\minidb.exe --data-dir data examples/basic.sql
```

```text
INSERT 1
INSERT 1
id	name	age
1	민수	25
2	O'Brien	30
```

메모리 검사(ASan/UBSan/누수)는 Docker Desktop을 켠 뒤 Linux 컨테이너에서 실행한다.

```text
docker run --rm -v "%cd%:/src" -w /src gcc:13 make sanitize
```

### 코드 구성

| 파일 | 역할 |
|---|---|
| src/main.c | CLI: 인자·SQL 파일 읽기, 문장 순차 실행, 결과·오류 출력, 종료 코드 |
| src/parser.c | Lexer + 재귀 하강 파서: SQL 텍스트 → Statement |
| src/executor.c | 값 검증 후 Storage에 저장·조회 요청. WHERE는 조회 콜백 앞의 필터로 검사, 결과는 콜백으로 전달 |
| src/storage.c | users.tsv 열기·검증·순차 읽기·추가, 다음 ID 복원. Phase 2: id 인덱스 구축·등록·`table_find_by_id` |
| src/bptree.c | (Phase 2) 메모리 B+ 트리: id → 레코드 바이트 위치 |
| tools/bpt_demo.c | (Phase 2) 트리 모양·탐색 경로를 보는 학습 도구 |
| tests/test_unit.c | Parser·Storage·Executor 단위 테스트 |
| tests/cli_test.py | 실제 프로세스를 실행하는 CLI 통합 테스트 |

이 폴더의 docs/를 DBMS 구현 기준으로 사용한다.

### 구현 담당에게 전달할 내용 (Phase 1 당시)

> 위 세 문서를 읽고 미정 결정을 먼저 확인한다. 결정 후 DB-P1-01(기본 저장·조회)만 구현하고 단위·CLI 통합 테스트를 실행한다. DB-P1-02(조건 검색·전체 검증)는 1단위 PASS 후 사용자가 진행을 지시하면 시작한다. Codex 검증을 기다리며 멈춘다. 함수별 학습 노트 대신 전체 흐름과 실행 예시를 설명한다. Phase 2·3과 선택 기능은 시작하지 않는다. 커밋·푸시는 사용자 지시 후 현재 작업 브랜치에서만 한다.
