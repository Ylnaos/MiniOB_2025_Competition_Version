-- 测试对包含聚合函数的视图进行 COUNT(*) 查询
-- 场景：视图本身包含聚合（无 GROUP BY，返回 1 行），外层对该视图进行 COUNT(*)

-- 清理环境
DROP TABLE IF EXISTS create_view_t1;
DROP VIEW IF EXISTS create_view_v7;

-- 创建测试表
CREATE TABLE create_view_t1(id INT, age INT, name CHAR(10));

-- 插入测试数据
INSERT INTO create_view_t1 VALUES (1, 10, 'a');
INSERT INTO create_view_t1 VALUES (2, 20, 'b');
INSERT INTO create_view_t1 VALUES (3, 30, 'c');

-- 创建包含聚合函数的视图（无 GROUP BY，返回 1 行）
CREATE VIEW create_view_v7 AS
  SELECT COUNT(t1.id) AS num, SUM(t1.age)+SUM(t2.age) AS data
  FROM create_view_t1 t1, create_view_t1 t2
  WHERE t1.id=t2.id;

-- 直接查询视图，应该返回 1 行
SELECT * FROM create_view_v7;

-- 关键测试：对视图进行 COUNT(*)，应该返回 1
-- 这是因为视图返回 1 行聚合结果，COUNT(*) 应该数这 1 行
SELECT COUNT(*) FROM create_view_v7;

-- 清理
DROP VIEW create_view_v7;
DROP TABLE create_view_t1;
