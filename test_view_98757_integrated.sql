-- 集成测试用例: 视图字段访问控制 + 计算字段功能
-- 表名: t98757, 视图名: v98757系列

-- ==================== 测试Part 1: 视图字段访问控制 ====================
-- 测试目标: 确保视图只暴露SELECT列表中的字段,不暴露底层表的其他字段

-- 创建测试表 (包含name字段)
CREATE TABLE create_view_t11522(id INT, name CHAR(20), age INT);

-- 插入测试数据
INSERT INTO create_view_t11522 VALUES(1, 'Alice', 20);
INSERT INTO create_view_t11522 VALUES(2, 'Bob', 25);
INSERT INTO create_view_t11522 VALUES(3, 'Charlie', 30);
INSERT INTO create_view_t11522 VALUES(4, 'David', 35);
INSERT INTO create_view_t11522 VALUES(5, 'Eve', 40);

-- 创建视图: 只暴露 id, age, data 三个字段 (不包含name)
CREATE VIEW create_view_v522 AS SELECT id, age, id+age AS data FROM create_view_t11522;

-- 测试1: 视图的合法字段访问 (应该成功)
SELECT COUNT(*) FROM create_view_v522;
SELECT COUNT(id) FROM create_view_v522;
SELECT SUM(age) FROM create_view_v522;
SELECT SUM(data) FROM create_view_v522;
SELECT * FROM create_view_v522;

-- 测试2: 访问视图未暴露的字段 (应该失败 - FAILURE)
SELECT COUNT(name) FROM create_view_v522;

-- 测试3: 访问不存在的字段 (应该失败 - FAILURE)
SELECT COUNT(score) FROM create_view_v522;

-- 清理Part 1
DROP VIEW create_view_v522;
DROP TABLE create_view_t11522;

-- ==================== 测试Part 2: 视图计算字段功能 ====================
-- 测试目标: 确保视图的计算字段能够正常工作

-- 创建测试表
CREATE TABLE t98757(id INT, age INT, score INT);

-- 插入测试数据
INSERT INTO t98757 VALUES(1, 20, 85);
INSERT INTO t98757 VALUES(2, 25, 90);
INSERT INTO t98757 VALUES(3, 30, 75);
INSERT INTO t98757 VALUES(4, 35, 95);

-- 测试1: 创建带计算字段的视图(没有明确定义列名)
CREATE VIEW v98757 AS SELECT id, age, id+age AS data FROM t98757;

-- 测试视图的基本查询
SELECT * FROM v98757;
SELECT COUNT(*) FROM v98757;
SELECT COUNT(id) FROM v98757;
SELECT SUM(age) FROM v98757;

-- 关键测试: 查询计算字段 data
SELECT SUM(data) FROM v98757;
SELECT COUNT(data) FROM v98757;
SELECT data FROM v98757;

-- 测试2: 创建带明确列名定义的视图
CREATE VIEW v98757_named(vid, vage, vdata) AS SELECT id, age, id+age FROM t98757;

-- 测试明确列名的视图
SELECT * FROM v98757_named;
SELECT SUM(vdata) FROM v98757_named;
SELECT COUNT(vdata) FROM v98757_named;

-- 测试3: 多个计算字段
CREATE VIEW v98757_multi AS SELECT id, age, score, id+age AS sum1, age+score AS sum2, id*age AS prod FROM t98757;

SELECT * FROM v98757_multi;
SELECT SUM(sum1) FROM v98757_multi;
SELECT SUM(sum2) FROM v98757_multi;
SELECT SUM(prod) FROM v98757_multi;

-- 测试4: 确保v98757不暴露底层表的score字段
SELECT COUNT(score) FROM v98757;

-- 清理Part 2
DROP VIEW v98757_multi;
DROP VIEW v98757_named;
DROP VIEW v98757;
DROP TABLE t98757;
