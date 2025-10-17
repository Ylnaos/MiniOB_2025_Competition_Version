-- 测试聚合视图的 COUNT(*) 查询
-- Bug: SELECT COUNT(*) FROM aggregate_view 返回错误结果
-- 期望: 总是返回 1 行(即使视图内部是空表或无匹配行)

-- ============================================
-- 测试用例 1: 空表的聚合
-- ============================================
DROP TABLE IF EXISTS empty_t;
CREATE TABLE empty_t(id INT, age INT, name CHAR(10));

-- 空表聚合应该返回 1 行
SELECT COUNT(*) FROM empty_t;
-- 期望输出: 1 行, 值为 0

SELECT COUNT(*), SUM(age), MAX(id) FROM empty_t;
-- 期望输出: 1 行, (0, NULL, NULL)

-- ============================================
-- 测试用例 2: 无匹配的 JOIN 聚合
-- ============================================
DROP TABLE IF EXISTS t1;
DROP TABLE IF EXISTS t2;
CREATE TABLE t1(id INT, age INT);
CREATE TABLE t2(id INT, age INT);
INSERT INTO t1 VALUES (1, 20);
INSERT INTO t2 VALUES (2, 30);

SELECT COUNT(*), SUM(t1.age) FROM t1, t2 WHERE t1.id = t2.id;
-- 期望输出: 1 行, (0, NULL)

-- ============================================
-- 测试用例 3: 原始 Bug 用例
-- ============================================
DROP TABLE IF EXISTS create_view_t1;
DROP VIEW IF EXISTS create_view_v7;

CREATE TABLE create_view_t1(id INT, age INT, name CHAR(10));

CREATE VIEW create_view_v7 AS SELECT COUNT(t1.id) as num, SUM(t1.age)+SUM(t2.age) as data FROM create_view_t1 t1, create_view_t1 t2 WHERE t1.id=t2.id;

-- 视图查询应该返回 1 行 (num=0, data=NULL)
SELECT * FROM create_view_v7;
-- 期望输出: 1 行, (0, NULL)

-- 外层 COUNT(*) 应该统计到视图返回的 1 行
SELECT COUNT(*) FROM create_view_v7;
-- 期望输出: 1 行, 值为 1

-- ============================================
-- 测试用例 4: 聚合视图 + 外层聚合
-- ============================================
DROP VIEW IF EXISTS v_agg;
CREATE VIEW v_agg AS SELECT COUNT(*) as cnt FROM empty_t;

SELECT COUNT(*) FROM v_agg;
-- 期望输出: 1

SELECT * FROM v_agg;
-- 期望输出: 1 行, cnt=0

-- ============================================
-- 测试用例 5: 有数据的情况(确保不破坏正常功能)
-- ============================================
DROP TABLE IF EXISTS data_t;
CREATE TABLE data_t(id INT, age INT);
INSERT INTO data_t VALUES (1, 20);
INSERT INTO data_t VALUES (2, 30);
INSERT INTO data_t VALUES (3, 25);

SELECT COUNT(*) FROM data_t;
-- 期望输出: 1 行, 值为 3

DROP VIEW IF EXISTS v_data;
CREATE VIEW v_data AS SELECT COUNT(*) as cnt, SUM(age) as total FROM data_t;

SELECT * FROM v_data;
-- 期望输出: 1 行, (3, 75)

SELECT COUNT(*) FROM v_data;
-- 期望输出: 1

-- ============================================
-- 测试用例 6: 边界情况 - 复杂聚合表达式
-- ============================================
SELECT COUNT(*), MAX(id), MIN(age), AVG(age) FROM empty_t;
-- 期望输出: 1 行, (0, NULL, NULL, NULL)

-- ============================================
-- 清理
-- ============================================
DROP VIEW IF EXISTS create_view_v7;
DROP VIEW IF EXISTS v_agg;
DROP VIEW IF EXISTS v_data;
DROP TABLE IF EXISTS create_view_t1;
DROP TABLE IF EXISTS empty_t;
DROP TABLE IF EXISTS t1;
DROP TABLE IF EXISTS t2;
DROP TABLE IF EXISTS data_t;
