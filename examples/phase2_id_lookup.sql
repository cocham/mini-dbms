-- Phase 2 / P2-01: WHERE id = ? 는 B+ 트리에서 바이트 위치를 찾아 그 줄 하나만 읽는다.
INSERT INTO users (name, age) VALUES ('민수', 25);
INSERT INTO users (name, age) VALUES ('지영', 30);
INSERT INTO users (name, age) VALUES ('O''Brien', 25);
INSERT INTO users (name, age) VALUES ('현우', 40);
INSERT INTO users (name, age) VALUES ('Alice', 33);
SELECT * FROM users WHERE id = 3;   -- 인덱스 조회: 트리 → 위치 → 한 줄
SELECT * FROM users WHERE id = 99;  -- 인덱스에 없음: 파일을 읽지 않고 헤더만
SELECT * FROM users WHERE age = 25; -- 선형 탐색: Phase 1과 같은 경로
