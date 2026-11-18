-- 첫 문장은 반영되고, 둘째 문장(값 오류)에서 멈춘다
UPDATE users SET age = 50 WHERE id = 2;
UPDATE users SET age = 200 WHERE id = 4;
UPDATE users SET age = 60 WHERE id = 4;
