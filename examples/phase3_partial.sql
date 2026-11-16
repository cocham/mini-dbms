-- Phase 3 실습용: 2번째 문장이 실행 단계에서 실패한다 (age 범위).
-- 1번째 INSERT는 저장되고, 3번째는 실행되지 않는다. 응답의 error.statement와 results를 보자.
INSERT INTO users (name, age) VALUES ('성공', 20);
INSERT INTO users (name, age) VALUES ('실패', 200);
INSERT INTO users (name, age) VALUES ('실행 안 됨', 30);
