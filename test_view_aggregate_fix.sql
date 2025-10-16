-- 测试视图聚合表达式修复
-- 验证 SUM(t1.age) + SUM(t2.age) 等复杂聚合表达式能正常工作

-- 初始化
DROP TABLE IF EXISTS test_t1;
DROP TABLE IF EXISTS test_t2;
DROP VIEW IF EXISTS test_view_complex;

-- 创建测试表
CREATE TABLE test_t1(id INT, age INT, name CHAR(10));
CREATE TABLE test_t2(id INT, age INT, name CHAR(10));

-- 插入测试数据
INSERT INTO test_t1 VALUES(1, 20, 'Alice');
INSERT INTO test_t1 VALUES(2, 25, 'Bob');
INSERT INTO test_t1 VALUES(3, 30, 'Charlie');

INSERT INTO test_t2 VALUES(1, 22, 'David');
INSERT INTO test_t2 VALUES(2, 27, 'Eve');

-- ========== 测试1：包含算术表达式的聚合视图 ==========
CREATE VIEW test_view_complex AS SELECT COUNT(t1.id) AS num,SUM(t1.age) AS sum1,SUM(t2.age) AS sum2,SUM(t1.age) + SUM(t2.age) AS total FROM test_t1 t1, test_t1 t2 WHERE t1.id = t2.id;

-- 测试1.1：查询视图内容（关键测试）
SELECT * FROM test_view_complex;

-- 预期结果：
-- NUM | SUM1 | SUM2 | TOTAL
-- 3   | 75   | 75   | 150

-- 测试1.2：对视图进行聚合
SELECT SUM(num), SUM(total) FROM test_view_complex;

-- 预期结果：
-- SUM(NUM) | SUM(TOTAL)
-- 3        | 150

-- 测试1.3：验证 COUNT(*) 返回1（聚合视图只有一行）
SELECT COUNT(*) FROM test_view_complex;

-- 预期结果：
-- COUNT(*)
-- 1

DROP VIEW test_view_complex;

-- ========== 测试2：更复杂的算术表达式 ==========
CREATE VIEW test_view_calc AS SELECT SUM(t1.age) * 2 AS double_sum,SUM(t1.age) + SUM(t2.age) * 3 AS complex_calc FROM test_t1 t1, test_t1 t2 WHERE t1.id = t2.id;

SELECT * FROM test_view_calc;

-- 预期结果：
-- DOUBLE_SUM | COMPLEX_CALC
-- 150        | 300

DROP VIEW test_view_calc;

-- ========== 测试3：包含 COUNT 和 SUM 的混合表达式 ==========
CREATE VIEW test_view_mixed AS SELECT COUNT(*) AS cnt,SUM(age) AS total,SUM(age) / COUNT(*) AS avg_manual FROM test_t1;

SELECT * FROM test_view_mixed;

-- 预期结果：
-- CNT | TOTAL | AVG_MANUAL
-- 3   | 75    | 25

DROP VIEW test_view_mixed;

-- 清理
DROP TABLE test_t1;
DROP TABLE test_t2;
