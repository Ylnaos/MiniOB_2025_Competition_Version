-- U87测试：CREATE VIEW聚合和自连接问题
-- 测试视图中的COUNT、SUM聚合函数和自连接

-- ========== 初始化环境 ==========
DROP TABLE IF EXISTS create_view_t1;
DROP TABLE IF EXISTS create_view_t2;
DROP VIEW IF EXISTS create_view_v7;

-- 创建测试表
CREATE TABLE create_view_t1(id INT, age INT, name CHAR(10));
CREATE TABLE create_view_t2(id INT, age INT, name CHAR(10));

-- 插入测试数据
INSERT INTO create_view_t1 VALUES(1, 20, 'Alice');
INSERT INTO create_view_t1 VALUES(2, 25, 'Bob');
INSERT INTO create_view_t1 VALUES(3, 30, 'Charlie');

INSERT INTO create_view_t2 VALUES(1, 22, 'David');
INSERT INTO create_view_t2 VALUES(2, 27, 'Eve');

-- ========== 场景1：自连接聚合视图（官方失败用例） ==========
-- 创建包含自连接和聚合的视图
CREATE VIEW create_view_v7 AS SELECT COUNT(t1.id) AS num, SUM(t1.age)+SUM(t2.age) AS data FROM create_view_t1 t1, create_view_t1 t2 WHERE t1.id=t2.id;

-- 测试1.1：查询视图的COUNT（预期返回1）
SELECT COUNT(*) FROM create_view_v7;

-- 测试1.2：直接查询视图内容
SELECT * FROM create_view_v7;

-- 测试1.3：验证原始查询
SELECT COUNT(t1.id) AS num, SUM(t1.age)+SUM(t2.age) AS data FROM create_view_t1 t1, create_view_t1 t2 WHERE t1.id=t2.id;

DROP VIEW create_view_v7;

-- ========== 场景2：简单聚合视图 ==========
CREATE VIEW create_view_v_simple AS SELECT COUNT(*) AS cnt, SUM(age) AS total_age FROM create_view_t1;

-- 测试2.1：查询简单聚合视图
SELECT * FROM create_view_v_simple;

-- 测试2.2：对视图再次聚合
SELECT COUNT(*) FROM create_view_v_simple;

DROP VIEW create_view_v_simple;

-- ========== 场景3：JOIN聚合视图 ==========
CREATE VIEW create_view_v_join AS SELECT t1.id, t1.age + t2.age AS combined_age FROM create_view_t1 t1, create_view_t2 t2 WHERE t1.id = t2.id;

-- 测试3.1：查询JOIN视图
SELECT * FROM create_view_v_join;

-- 测试3.2：对JOIN视图聚合
SELECT COUNT(*) FROM create_view_v_join;

-- 测试3.3：对JOIN视图的聚合查询
SELECT SUM(combined_age) FROM create_view_v_join;

DROP VIEW create_view_v_join;

-- ========== 场景4：嵌套聚合视图 ==========
CREATE VIEW create_view_v_nested AS SELECT COUNT(*) AS cnt FROM create_view_t1 WHERE age > 20;

-- 测试4.1：查询嵌套聚合视图
SELECT * FROM create_view_v_nested;

-- 测试4.2：对已聚合的视图再次COUNT
SELECT COUNT(*) FROM create_view_v_nested;

DROP VIEW create_view_v_nested;

-- ========== 场景5：复杂表达式聚合视图 ==========
CREATE VIEW create_view_v_complex AS SELECT COUNT(t1.id) AS num,SUM(t1.age) AS sum1,SUM(t2.age) AS sum2,SUM(t1.age) + SUM(t2.age) AS total FROM create_view_t1 t1, create_view_t1 t2 WHERE t1.id = t2.id;

-- 测试5.1：查询复杂聚合视图
SELECT * FROM create_view_v_complex;

-- 测试5.2：验证COUNT结果
SELECT COUNT(*) FROM create_view_v_complex;

-- 测试5.3：对视图字段聚合
SELECT SUM(num), SUM(total) FROM create_view_v_complex;

DROP VIEW create_view_v_complex;

-- ========== 清理 ==========
DROP TABLE create_view_t1;
DROP TABLE create_view_t2;
