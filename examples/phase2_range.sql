-- Phase 2 / P2-03: id 범위 조회는 시작 리프만 B+ 트리로 찾고, 리프 연결을 따라 범위 안의 줄만 읽는다.
INSERT INTO users (name, age) VALUES ('민수', 25);
INSERT INTO users (name, age) VALUES ('지영', 30);
INSERT INTO users (name, age) VALUES ('O''Brien', 25);
INSERT INTO users (name, age) VALUES ('현우', 40);
INSERT INTO users (name, age) VALUES ('Alice', 33);
SELECT * FROM users WHERE id BETWEEN 2 AND 4;   -- 양 끝 포함: 2, 3, 4
SELECT * FROM users WHERE id > 3;               -- 4, 5
SELECT * FROM users WHERE id BETWEEN 4 AND 2;   -- 뒤집힌 범위: 오류 없이 결과 없음
EXPLAIN SELECT * FROM users WHERE id BETWEEN 2 AND 4;
EXPLAIN SELECT * FROM users WHERE id >= 3;
