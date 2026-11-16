# 미니 DBMS 빌드 (DB-D04)
#   Windows: mingw32-make (TDM-GCC)  — 빌드·단위·CLI 테스트
#   macOS: make (Apple clang)        — 빌드·단위·CLI 테스트 (Phase 2부터 주 개발 환경)
#   Linux/Docker: make               — 위 + sanitize(ASan/UBSan·누수 검사), tsan, race-demo(Phase 3)

CC = gcc
CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -O2 -g
SAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -O1 -g
TSAN_FLAGS = -fsanitize=thread -fno-omit-frame-pointer -O1 -g
# DB 진입점(db.c)이 pthread mutex를 쓴다. Windows는 TDM-GCC의 winpthreads (Phase 3, DB-P3-D04)
THREAD_FLAGS = -pthread

SRC = src/common.c src/parser.c src/bptree.c src/storage.c src/executor.c src/db.c
HDR = src/common.h src/parser.h src/bptree.h src/storage.h src/executor.h src/db.h

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

.PHONY: all test unit cli sanitize tsan race-demo bench clean

all: $(BIN) $(DEMO_BIN)

build:
	mkdir build

$(BIN): $(SRC) src/main.c $(HDR) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) $(THREAD_FLAGS) -o $@ $(SRC) src/main.c

# 학습용: B+ 트리 모양과 탐색 경로를 출력한다 (Phase 2)
$(DEMO_BIN): src/bptree.c src/bptree.h tools/bpt_demo.c | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ src/bptree.c tools/bpt_demo.c

$(TEST_BIN): $(SRC) tests/test_unit.c $(HDR) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) $(THREAD_FLAGS) -DMINIDB_TESTING -Isrc -o $@ $(SRC) tests/test_unit.c

test: unit cli

unit: $(TEST_BIN)
	$(call fix,$(TEST_BIN)) build/tmp_unit

cli: $(BIN)
	$(PYTHON) tests/cli_test.py $(BIN)

# Linux 전용: ASan/UBSan과 LeakSanitizer로 단위·CLI 테스트를 다시 실행한다.
sanitize:
	mkdir -p build/san
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(THREAD_FLAGS) -o build/san/minidb $(SRC) src/main.c
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(THREAD_FLAGS) -DMINIDB_TESTING -Isrc -o build/san/test_unit $(SRC) tests/test_unit.c
	ASAN_OPTIONS=detect_leaks=1 build/san/test_unit build/san/tmp_unit
	ASAN_OPTIONS=detect_leaks=1 $(PYTHON) tests/cli_test.py build/san/minidb
	$(CC) $(CFLAGS) $(SAN_FLAGS) -Isrc -o build/san/bpt_demo src/bptree.c tools/bpt_demo.c
	ASAN_OPTIONS=detect_leaks=1 build/san/bpt_demo --shuffle 5 500 1 250 500 999 > /dev/null

# Linux 전용: ThreadSanitizer로 단위 테스트(동시 정확성 테스트 포함)를 다시 실행한다. 보고가 있으면 실패.
#   최근 커널(mmap 무작위 비트 32)에서는 GCC 13의 TSan이 주소 배치를 받아들이지 못해 시작하지 못한다.
#   그래서 주소 무작위화를 끄고(setarch -R) 실행한다. Docker 기본 seccomp가 이를 막으므로
#   docker run --security-opt seccomp=unconfined ... make tsan 으로 실행한다.
NOASLR = setarch $$(uname -m) -R
tsan:
	mkdir -p build/tsan
	$(CC) $(CFLAGS) $(TSAN_FLAGS) $(THREAD_FLAGS) -DMINIDB_TESTING -Isrc -o build/tsan/test_unit $(SRC) tests/test_unit.c
	TSAN_OPTIONS="halt_on_error=1 exitcode=66" $(NOASLR) build/tsan/test_unit build/tsan/tmp_unit

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

# 100만 건 생성·삽입 후 인덱스 조회 vs 선형 탐색 측정 (P2-04). 느리므로 test에 넣지 않는다.
#   make bench BENCH_ROWS=100000 처럼 건수를 줄여 볼 수 있다. 결과는 docs/phase2/benchmark.md로 옮긴다.
BENCH_ROWS ?= 1000000
bench: $(BIN)
	$(PYTHON) tools/bench.py --rows $(BENCH_ROWS) --bin $(BIN)

clean:
	$(RMDIR)
