-- 创建测试表
CREATE TABLE create_view_t11(id INT, name CHAR(20), age INT);

-- 插入测试数据
INSERT INTO create_view_t11 VALUES(1, 'Alice', 20);
INSERT INTO create_view_t11 VALUES(2, 'Bob', 25);
INSERT INTO create_view_t11 VALUES(3, 'Charlie', 30);
INSERT INTO create_view_t11 VALUES(4, 'David', 35);

-- 测试1: 创建带列名定义的视图 v2(只有id和age两列)
CREATE VIEW create_view_v22(id, age) AS SELECT id, age FROM create_view_t11;

-- 测试视图v2的查询
SELECT COUNT(*) FROM create_view_v22;
SELECT COUNT(id) FROM create_view_v22;
SELECT SUM(age) FROM create_view_v22;
SELECT COUNT(name) FROM create_view_v22;

-- 测试2: 创建带计算列的视图 v5
CREATE VIEW create_view_v55 AS SELECT id, age, id+age AS data FROM create_view_t11;

-- 测试视图v5的查询
SELECT COUNT(*) FROM create_view_v55;
SELECT COUNT(id) FROM create_view_v55;
SELECT SUM(age) FROM create_view_v55;
SELECT SUM(data) FROM create_view_v55;

-- 清理
DROP VIEW create_view_v22;
DROP VIEW create_view_v55;
DROP TABLE create_view_t11;