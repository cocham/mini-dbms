#!/usr/bin/env python3
"""대량 INSERT SQL 생성기 (Phase 2 / P2-04).

사용법: python tools/gen_sql.py N > build/million.sql

N개의 INSERT 문을 stdout에 쓴다. 실행할 때마다 같은 결과가 나온다(난수 없음).
  i번째 레코드: name = 'user' + 6자리 이상 번호, age = i % 151
빈 데이터 디렉터리에 넣으면 i번째 레코드의 id가 i가 된다(자동 증가).
age = 30인 레코드 수는 i % 151 == 30인 i의 개수다(N = 1,000,000이면 6,623건).
"""
import sys


def insert_lines(n):
    """i = 1..n의 INSERT 문을 한 줄씩 돌려준다."""
    for i in range(1, n + 1):
        yield "INSERT INTO users (name, age) VALUES ('user%06d', %d);\n" % (i, i % 151)


def write_sql(n, out):
    buf = []
    for line in insert_lines(n):
        buf.append(line)
        if len(buf) >= 10000:
            out.write("".join(buf))
            buf.clear()
    out.write("".join(buf))


def main():
    if len(sys.argv) != 2 or not sys.argv[1].isdigit():
        sys.stderr.write("사용법: gen_sql.py 개수\n")
        return 2
    # Windows에서도 LF로 쓴다(SQL 파서는 CRLF도 받지만 크기·속도를 플랫폼과 무관하게 맞춘다).
    out = open(sys.stdout.fileno(), "w", encoding="utf-8", newline="\n", closefd=False)
    write_sql(int(sys.argv[1]), out)
    out.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
