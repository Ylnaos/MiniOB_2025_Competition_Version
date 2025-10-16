-- 综合测试向量索引功能
-- 包含：基础功能、EXPLAIN、边界情况

-- ============================================
-- 测试1: 基础向量索引测试
-- ============================================
DROP TABLE IF EXISTS TEST25;
CREATE TABLE TEST25 (ID INT, C1 VECTOR(3));

-- 插入数据
INSERT INTO TEST25 VALUES(1, '[1,2,3]');
INSERT INTO TEST25 VALUES(2, '[1,2,4]');
INSERT INTO TEST25 VALUES(3, '[1,3,4]');
INSERT INTO TEST25 VALUES(4, '[2,3,4]');
INSERT INTO TEST25 VALUES(5, '[5,6,7]');

-- 创建向量索引
CREATE VECTOR INDEX V_I ON TEST25(C1) WITH(TYPE=IVFFLAT, DISTANCE=L2_DISTANCE, LISTS=1, PROBES=1);

-- 测试EXPLAIN
EXPLAIN SELECT C1, L2_DISTANCE(C1, '[1,2,3]') FROM TEST25 ORDER BY L2_DISTANCE(C1, '[1,2,3]') LIMIT 3;

-- 测试查询
SELECT ID, C1, L2_DISTANCE(C1, '[1,2,3]') AS DIST FROM TEST25 ORDER BY L2_DISTANCE(C1, '[1,2,3]') LIMIT 3;

-- ============================================
-- 测试2: 不同维度的向量
-- ============================================
DROP TABLE IF EXISTS TEST252;
CREATE TABLE TEST252 (ID INT, C1 VECTOR(10));

INSERT INTO TEST252 VALUES(1, '[1,2,3,4,5,6,7,8,9,10]');
INSERT INTO TEST252 VALUES(2, '[2,3,4,5,6,7,8,9,10,11]');
INSERT INTO TEST252 VALUES(3, '[0,0,0,0,0,0,0,0,0,0]');
INSERT INTO TEST252 VALUES(4, '[8.2,9.13,4.37,9.68,3.09,8.04,7.61,9.58,4.32,3.49]');

CREATE VECTOR INDEX V_I2 ON TEST252(C1) WITH(TYPE=IVFFLAT, DISTANCE=L2_DISTANCE, LISTS=1, PROBES=1);

EXPLAIN SELECT * FROM TEST252 ORDER BY L2_DISTANCE(C1, '[5,5,5,5,5,5,5,5,5,5]') LIMIT 2;

SELECT ID, L2_DISTANCE(C1, '[5,5,5,5,5,5,5,5,5,5]') AS DIST FROM TEST252 ORDER BY L2_DISTANCE(C1, '[5,5,5,5,5,5,5,5,5,5]') LIMIT 2;

-- ============================================
-- 测试3: 多个LIMIT值
-- ============================================
EXPLAIN SELECT * FROM TEST25 ORDER BY L2_DISTANCE(C1, '[2,2,2]') LIMIT 1;
SELECT ID, L2_DISTANCE(C1, '[2,2,2]') AS DIST FROM TEST25 ORDER BY L2_DISTANCE(C1, '[2,2,2]') LIMIT 1;

EXPLAIN SELECT * FROM TEST25 ORDER BY L2_DISTANCE(C1, '[2,2,2]') LIMIT 5;
SELECT ID, L2_DISTANCE(C1, '[2,2,2]') AS DIST FROM TEST25 ORDER BY L2_DISTANCE(C1, '[2,2,2]') LIMIT 5;

-- ============================================
-- 测试4: 空表创建索引
-- ============================================
DROP TABLE IF EXISTS EMPTY_TEST25;
CREATE TABLE EMPTY_TEST25 (ID INT, C1 VECTOR(3));
CREATE VECTOR INDEX V_EMPTY ON EMPTY_TEST25(C1) WITH(TYPE=IVFFLAT, DISTANCE=L2_DISTANCE, LISTS=1, PROBES=1);

-- 这个不应该崩溃，即使表是空的
EXPLAIN SELECT * FROM EMPTY_TEST25 ORDER BY L2_DISTANCE(C1, '[1,2,3]') LIMIT 3;

-- ============================================
-- 测试5: 不同查询向量
-- ============================================
-- 查询向量为零向量
SELECT ID, L2_DISTANCE(C1, '[0,0,0]') AS DIST FROM TEST25 ORDER BY L2_DISTANCE(C1, '[0,0,0]') LIMIT 3;

-- 查询向量为负值
SELECT ID, L2_DISTANCE(C1, '[-1,-1,-1]') AS DIST FROM TEST25 ORDER BY L2_DISTANCE(C1, '[-1,-1,-1]') LIMIT 2;

-- 查询向量为浮点数
SELECT ID, L2_DISTANCE(C1, '[1.5,2.5,3.5]') AS DIST FROM TEST25 ORDER BY L2_DISTANCE(C1, '[1.5,2.5,3.5]') LIMIT 2;

-- ============================================
-- 测试6: 组合测试 - 带投影的EXPLAIN
-- ============================================
EXPLAIN SELECT ID, C1 FROM TEST25 ORDER BY L2_DISTANCE(C1, '[1,2,3]') LIMIT 3;
EXPLAIN SELECT C1, L2_DISTANCE(C1, '[1,2,3]') FROM TEST25 ORDER BY L2_DISTANCE(C1, '[1,2,3]') LIMIT 3;
EXPLAIN SELECT * FROM TEST25 ORDER BY L2_DISTANCE(C1, '[1,2,3]') LIMIT 3;

-- ============================================
-- 预期行为：
-- 1. 所有EXPLAIN查询都应该成功返回查询计划
-- 2. 查询计划中应该包含"VECTOR_INDEX_SCAN(V_I ON TEST25)"
-- 3. 所有SELECT查询都应该正常执行并返回正确的结果
-- 4. 距离值应该是非负浮点数，保留两位小数
-- 5. 结果应该按距离从小到大排序
-- 6. 不应该出现"failed to receive response from observer"错误
-- ============================================
