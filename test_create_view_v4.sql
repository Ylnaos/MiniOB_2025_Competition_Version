-- 按照用户提供的测试用例精确测试 create_view_v4

-- 清理环境
DROP TABLE IF EXISTS create_view_t1;
DROP TABLE IF EXISTS create_view_t2;
DROP VIEW IF EXISTS create_view_v4;

-- 创建基础表 (按用户测试用例的顺序)
CREATE TABLE create_view_t1(id int, age int, name char(10));
CREATE TABLE create_view_t2(id int, age int, name char(10));

-- 创建多表视图 (使用 create_view_v4)
CREATE VIEW create_view_v4 AS select t1.id AS id, t1.age AS age, t2.name AS name FROM create_view_t1 t1, create_view_t2 t2 WHERE t1.id = t2.id;

-- 测试用户提供的具体INSERT语句
INSERT INTO create_view_v4 VALUES(69, 69, 'PAI2YH');
INSERT INTO create_view_v4(id, age) VALUES(195, 195);

-- 验证插入结果
SELECT * FROM create_view_t1 ORDER BY id;
SELECT * FROM create_view_t2 ORDER BY id;

-- 查询视图结果
SELECT * FROM create_view_v4;
SELECT count(*) FROM create_view_v4;
SELECT count(id) FROM create_view_v4;
SELECT sum(age) FROM create_view_v4;
SELECT count(name) FROM create_view_v4;

-- 清理环境
DROP VIEW IF EXISTS create_view_v4;
DROP TABLE IF EXISTS create_view_t1;
DROP TABLE IF EXISTS create_view_t2;