-- Phase 2 / P2-02: EXPLAIN은 실행 계획 한 줄만 출력하고 조회는 실행하지 않는다.
-- 괄호 안 숫자는 계획 시점 통계(트리 높이, 전체 행 수)이며 실행 측정값이 아니다.
INSERT INTO users (name, age) VALUES ('민수', 25);
INSERT INTO users (name, age) VALUES ('지영', 30);
INSERT INTO users (name, age) VALUES ('O''Brien', 25);
EXPLAIN SELECT * FROM users WHERE id = 3;   -- INDEX LOOKUP: B+ 트리 높이만큼 노드를 거친다
EXPLAIN SELECT * FROM users WHERE id = 99;  -- 없는 id도 경로는 같다(존재 여부는 실행해야 안다)
EXPLAIN SELECT * FROM users WHERE age = 25; -- FULL SCAN: 전체 행을 읽는다
EXPLAIN SELECT * FROM users;                -- FULL SCAN: 조건 없음
SELECT * FROM users WHERE id = 3;           -- 같은 조건의 실제 조회는 계획대로 실행된다
