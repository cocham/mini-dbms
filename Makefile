# 미니 DBMS 빌드 (DB-D04)
#   Windows: mingw32-make (TDM-GCC)  — 빌드·단위·CLI 테스트
#   macOS: make (Apple clang)        — 빌드·단위·CLI 테스트 (Phase 2부터 주 개발 환경)
#   Linux/Docker: make               — 위 + API 서버·서버 테스트, sanitize(ASan/UBSan·누수 검사), tsan, race-demo(Phase 3)

CC = gcc
CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -O2 -g
SAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -O1 -g
TSAN_FLAGS = -fsanitize=thread -fno-omit-frame-pointer -O1 -g
# DB 진입점(db.c)이 pthread mutex를 쓴다. Windows는 TDM-GCC의 winpthreads (Phase 3, DB-P3-D04)
THREAD_FLAGS = -pthread

SRC = src/common.c src/parser.c src/bptree.c src/storage.c src/executor.c src/db.c src/json.c src/workqueue.c
HDR = src/common.h src/parser.h src/bptree.h src/storage.h src/executor.h src/db.h src/json.h src/workqueue.h
# API 서버 (Phase 3, POSIX 전용). Windows에서는 빌드하지 않는다 (DB-P3-D02)
SERVER_SRC = src/http.c src/server.c
SERVER_HDR = src/http.h

ifeq ($(OS),Windows_NT)
  SHELL := cmd.exe
  EXE := .exe
  PYTHON ?= python
  CPPFLAGS += -D__USE_MINGW_ANSI_STDIO=1
  fix = $(subst /,\,$1)
  RMDIR = if exist build rmdir /s /q build
else
  EXE :=
  PYTHON ?= python3
  fix = $1
  RMDIR = rm -rf build
endif

BIN = build/minidb$(EXE)
TEST_BIN = build/test_unit$(EXE)
DEMO_BIN = build/bpt_demo$(EXE)

SERVER_BIN = build/minidb-server
SERVER_TEST_BIN = build/minidb-server-test
LOADGEN_BIN = build/loadgen

.PHONY: all test unit cli server sanitize tsan race-demo read-bench load-bench bench clean

ifeq ($(OS),Windows_NT)
all: $(BIN) $(DEMO_BIN)
test: unit cli
else
all: $(BIN) $(DEMO_BIN) $(SERVER_BIN)
test: unit cli server
endif

build:
	mkdir build

$(BIN): $(SRC) src/main.c $(HDR) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) $(THREAD_FLAGS) -o $@ $(SRC) src/main.c

# 학습용: B+ 트리 모양과 탐색 경로를 출력한다 (Phase 2)
$(DEMO_BIN): src/bptree.c src/bptree.h tools/bpt_demo.c | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ src/bptree.c tools/bpt_demo.c

$(TEST_BIN): $(SRC) tests/test_unit.c $(HDR) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) $(THREAD_FLAGS) -DMINIDB_TESTING -Isrc -o $@ $(SRC) tests/test_unit.c

$(SERVER_BIN): $(SRC) $(SERVER_SRC) $(HDR) $(SERVER_HDR) | build
	$(CC) $(CFLAGS) $(THREAD_FLAGS) -o $@ $(SRC) $(SERVER_SRC)

# 테스트 전용 서버: MINIDB_TEST_FAULT=index|write로 저장 후 실패를 재현한다. 배포용이 아니다.
$(SERVER_TEST_BIN): $(SRC) $(SERVER_SRC) $(HDR) $(SERVER_HDR) | build
	$(CC) $(CFLAGS) $(THREAD_FLAGS) -DMINIDB_TESTING -Isrc -o $@ $(SRC) $(SERVER_SRC)

unit: $(TEST_BIN)
	$(call fix,$(TEST_BIN)) build/tmp_unit

cli: $(BIN)
	$(PYTHON) tests/cli_test.py $(BIN)

server: $(SERVER_BIN) $(SERVER_TEST_BIN) $(BIN) $(LOADGEN_BIN)
	$(PYTHON) tests/server_test.py $(SERVER_BIN) $(SERVER_TEST_BIN) $(BIN)
	$(PYTHON) tests/loadgen_test.py $(LOADGEN_BIN)

# 부하 생성기 (P3-05, POSIX). 측정 도구지만 집계가 틀리면 측정이 틀리므로 make test에서 가짜 서버로 검사한다.
$(LOADGEN_BIN): tools/loadgen.c | build
	$(CC) $(CFLAGS) $(THREAD_FLAGS) -o $@ tools/loadgen.c

# Linux 전용: ASan/UBSan과 LeakSanitizer로 단위·CLI 테스트를 다시 실행한다.
sanitize:
	mkdir -p build/san
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(THREAD_FLAGS) -o build/san/minidb $(SRC) src/main.c
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(THREAD_FLAGS) -DMINIDB_TESTING -Isrc -o build/san/test_unit $(SRC) tests/test_unit.c
	ASAN_OPTIONS=detect_leaks=1 build/san/test_unit build/san/tmp_unit
	ASAN_OPTIONS=detect_leaks=1 $(PYTHON) tests/cli_test.py build/san/minidb
	$(CC) $(CFLAGS) $(SAN_FLAGS) -Isrc -o build/san/bpt_demo src/bptree.c tools/bpt_demo.c
	ASAN_OPTIONS=detect_leaks=1 build/san/bpt_demo --shuffle 5 500 1 250 500 999 > /dev/null
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(THREAD_FLAGS) -o build/san/minidb-server $(SRC) $(SERVER_SRC)
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(THREAD_FLAGS) -DMINIDB_TESTING -Isrc -o build/san/minidb-server-test $(SRC) $(SERVER_SRC)
	ASAN_OPTIONS=detect_leaks=1 $(PYTHON) tests/server_test.py build/san/minidb-server build/san/minidb-server-test build/san/minidb
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(THREAD_FLAGS) -o build/san/loadgen tools/loadgen.c
	ASAN_OPTIONS=detect_leaks=1 $(PYTHON) tests/loadgen_test.py build/san/loadgen

# Linux 전용: ThreadSanitizer로 단위 테스트(동시 정확성 테스트 포함)를 다시 실행한다. 보고가 있으면 실패.
#   최근 커널(mmap 무작위 비트 32)에서는 GCC 13의 TSan이 주소 배치를 받아들이지 못해 시작하지 못한다.
#   그래서 주소 무작위화를 끄고(setarch -R) 실행한다. Docker 기본 seccomp가 이를 막으므로
#   docker run --security-opt seccomp=unconfined ... make tsan 으로 실행한다.
NOASLR = setarch $$(uname -m) -R
tsan:
	mkdir -p build/tsan
	$(CC) $(CFLAGS) $(TSAN_FLAGS) $(THREAD_FLAGS) -DMINIDB_TESTING -Isrc -o build/tsan/test_unit $(SRC) tests/test_unit.c
	TSAN_OPTIONS="halt_on_error=1 exitcode=66" $(NOASLR) build/tsan/test_unit build/tsan/tmp_unit
	$(CC) $(CFLAGS) $(TSAN_FLAGS) $(THREAD_FLAGS) -o build/tsan/minidb-server $(SRC) $(SERVER_SRC)
	$(CC) $(CFLAGS) $(TSAN_FLAGS) $(THREAD_FLAGS) -DMINIDB_TESTING -Isrc -o build/tsan/minidb-server-test $(SRC) $(SERVER_SRC)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) -o build/tsan/minidb $(SRC) src/main.c
	TSAN_OPTIONS="halt_on_error=1 exitcode=66" MINIDB_SERVER_WRAP="$(NOASLR)" $(PYTHON) tests/server_test.py build/tsan/minidb-server build/tsan/minidb-server-test build/tsan/minidb

# Linux 전용 실험(판정 없음, test에 넣지 않음): 잠금을 끈 엔진을 여러 스레드로 불러 경쟁이 나타나는지 관찰한다.
#   결과는 실행마다 다르다. 이상이 안 나타나는 실행도 정상이다. 마지막에 같은 빌드를 TSan으로 한 번 돌린다.
#   race_demo는 실행할 때마다 자기 데이터 파일(build/race/data/users.tsv)을 비우고 시작한다.
RACE_RUNS ?= 10
race-demo:
	mkdir -p build/race
	$(CC) $(CFLAGS) $(THREAD_FLAGS) -DMINIDB_NO_LOCK -Isrc -o build/race/race_demo $(SRC) tools/race_demo.c
	$(CC) $(CFLAGS) $(TSAN_FLAGS) $(THREAD_FLAGS) -DMINIDB_NO_LOCK -Isrc -o build/race/race_demo_tsan $(SRC) tools/race_demo.c
	@for i in $$(seq 1 $(RACE_RUNS)); do \
		printf '실행 %2d: ' $$i; \
		build/race/race_demo build/race/data || echo "비정상 종료 (코드 $$?)"; \
	done
	@echo '--- 같은 잠금 없는 빌드를 ThreadSanitizer로 한 번 (보고 요약) ---'
	@TSAN_OPTIONS="halt_on_error=0 report_signal_unsafe=0" $(NOASLR) build/race/race_demo_tsan build/race/data > build/race/tsan_out.txt 2> build/race/tsan.txt || true
	@cat build/race/tsan_out.txt
	@grep -m1 "FATAL" build/race/tsan.txt || true
	@printf 'TSan 경고 수: '; grep -c "WARNING: ThreadSanitizer" build/race/tsan.txt || true
	@grep -E -m8 "WARNING: ThreadSanitizer|^    #0 " build/race/tsan.txt || true
	@echo '(전체 보고: build/race/tsan.txt)'

# Linux 전용 측정(판정 없음, test에 넣지 않음, P3-04): 같은 데이터로 전역 잠금 빌드와 읽기·쓰기 잠금 빌드를
#   비교한다. 선형 탐색을 동시에 보냈을 때 전체 시간, 탐색이 도는 동안 INSERT 응답 시간(writer 기아).
#   make read-bench READ_ROWS=200000 READ_CLIENTS=8 처럼 바꿔 볼 수 있다. 결과는 CPU 수에 따라 다르다.
READ_ROWS ?= 1000000
READ_CLIENTS ?= 4
read-bench: $(SERVER_BIN)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) -DMINIDB_GLOBAL_LOCK -o build/minidb-server-global $(SRC) $(SERVER_SRC)
	$(PYTHON) tools/read_parallel.py --rw $(SERVER_BIN) --global build/minidb-server-global \
		--rows $(READ_ROWS) --clients $(READ_CLIENTS) --threads $(READ_CLIENTS)

# Linux 전용 부하 측정(판정 없음, test에 넣지 않음, P3-05): 워커 수, 요청 종류 × 잠금 방식, 큐 크기별
#   처리량과 응답 시간 분포. 결과는 build/load_result.md에도 쓴다. docs/phase3/benchmark.md로 옮긴다.
#   측정 사이에 TIME_WAIT 연결이 빠질 때까지 기다리므로 몇 분 걸린다(tools/load.py 설명).
#   make load-bench LOAD_ROWS=200000 처럼 줄여 볼 수 있다.
LOAD_ROWS ?= 1000000
LOAD_SECONDS ?= 2
load-bench: $(SERVER_BIN) $(BIN) $(LOADGEN_BIN)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) -DMINIDB_GLOBAL_LOCK -o build/minidb-server-global $(SRC) $(SERVER_SRC)
	$(PYTHON) tools/load.py --server $(SERVER_BIN) --global build/minidb-server-global --cli $(BIN) \
		--loadgen $(LOADGEN_BIN) --rows $(LOAD_ROWS) --seconds $(LOAD_SECONDS)

# 100만 건 생성·삽입 후 인덱스 조회 vs 선형 탐색 측정 (P2-04). 느리므로 test에 넣지 않는다.
#   make bench BENCH_ROWS=100000 처럼 건수를 줄여 볼 수 있다. 결과는 docs/phase2/benchmark.md로 옮긴다.
BENCH_ROWS ?= 1000000
bench: $(BIN)
	$(PYTHON) tools/bench.py --rows $(BENCH_ROWS) --bin $(BIN)

clean:
	$(RMDIR)
