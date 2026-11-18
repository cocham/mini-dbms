-- 가장 큰 id(5)를 지운 뒤 INSERT는 몇 번을 받는가
DELETE FROM users WHERE id = 5;
INSERT INTO users (name, age) VALUES ('han', 22);
SELECT * FROM users WHERE id >= 4;
