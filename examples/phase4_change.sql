-- 고치고 지우기: 결과 줄의 숫자는 조건에 맞은 행 수
UPDATE users SET age = 31 WHERE age = 30;
UPDATE users SET name = 'O''Brien' WHERE id = 1;
DELETE FROM users WHERE id = 3;
UPDATE users SET age = 99 WHERE id = 3;
SELECT * FROM users;
