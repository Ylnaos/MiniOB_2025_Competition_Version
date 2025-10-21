-- 简化的create_view_v4测试脚本
DROP TABLE IF EXISTS create_view_t1;
DROP TABLE IF EXISTS create_view_t2;
CREATE TABLE create_view_t1(id int, age int, name char(10));
CREATE TABLE create_view_t2(id int, age int, name char(10));
CREATE VIEW create_view_v4 AS select t1.id AS id, t1.age AS age, t2.name AS name FROM create_view_t1 t1, create_view_t2 t2 WHERE t1.id=t2.id;
INSERT INTO create_view_v4 VALUES(69, 69, 'PAI2YH');
INSERT INTO create_view_v4(id, age) VALUES(195, 195);
SELECT 'test completed' as result;