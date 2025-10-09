-- 测试视图计算字段问题
-- 创建测试表
CREATE TABLE t98755(id INT, age INT, score INT);

-- 插入测试数据
INSERT INTO t98755 VALUES(1, 20, 85);
INSERT INTO t98755 VALUES(2, 25, 90);
INSERT INTO t98755 VALUES(3, 30, 75);
INSERT INTO t98755 VALUES(4, 35, 95);

-- 测试1: 创建带计算字段的视图(没有明确定义列名)
CREATE VIEW v98755 AS SELECT id, age, id+age AS data FROM t98755;

-- 测试视图的基本查询
SELECT * FROM v98755;
SELECT COUNT(*) FROM v98755;
SELECT COUNT(id) FROM v98755;
SELECT SUM(age) FROM v98755;

-- 关键测试: 查询计算字段 data
SELECT SUM(data) FROM v98755;
SELECT COUNT(data) FROM v98755;
SELECT data FROM v98755;

-- 测试2: 创建带明确列名定义的视图
CREATE VIEW v98755_named(vid, vage, vdata) AS SELECT id, age, id+age FROM t98755;

-- 测试明确列名的视图
SELECT * FROM v98755_named;
SELECT SUM(vdata) FROM v98755_named;
SELECT COUNT(vdata) FROM v98755_named;

-- 测试3: 多个计算字段
CREATE VIEW v98755_multi AS SELECT id, age, score, id+age AS sum1, age+score AS sum2, id*age AS prod FROM t98755;

SELECT * FROM v98755_multi;
SELECT SUM(sum1) FROM v98755_multi;
SELECT SUM(sum2) FROM v98755_multi;
SELECT SUM(prod) FROM v98755_multi;

-- 清理
DROP VIEW v98755_multi;
DROP VIEW v98755_named;
DROP VIEW v98755;
DROP TABLE t98755;
