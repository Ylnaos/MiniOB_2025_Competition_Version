-- 向量距离函数边界测试用例
-- 测试 L2_DISTANCE 在各种边界情况下的表现

-- 清理测试环境
DROP TABLE IF EXISTS T_VEC_BOUNDARY;

-- 创建测试表
CREATE TABLE T_VEC_BOUNDARY(ID INT,VEC3 VECTOR(3),VEC10 VECTOR(10));

-- ==================== 测试1: 空表查询 ====================
-- 预期：返回空结果，不崩溃
SELECT * FROM T_VEC_BOUNDARY ORDER BY L2_DISTANCE(VEC3, '[1,2,3]') LIMIT 1;

-- ==================== 测试2: 插入正常数据 ====================
INSERT INTO T_VEC_BOUNDARY VALUES(1, '[1,2,3]', '[1,2,3,4,5,6,7,8,9,10]');
INSERT INTO T_VEC_BOUNDARY VALUES(2, '[4,5,6]', '[10,9,8,7,6,5,4,3,2,1]');
INSERT INTO T_VEC_BOUNDARY VALUES(3, '[7,8,9]', '[5,5,5,5,5,5,5,5,5,5]');

-- 测试：基本查询
SELECT * FROM T_VEC_BOUNDARY ORDER BY L2_DISTANCE(VEC3, '[1,2,3]') LIMIT 1;
SELECT ID, L2_DISTANCE(VEC3, '[1,2,3]') AS DIST FROM T_VEC_BOUNDARY ORDER BY DIST LIMIT 3;

-- ==================== 测试3: NULL 值处理 ====================
INSERT INTO T_VEC_BOUNDARY VALUES(4, NULL, '[1,2,3,4,5,6,7,8,9,10]');
INSERT INTO T_VEC_BOUNDARY VALUES(5, '[1,2,3]', NULL);
INSERT INTO T_VEC_BOUNDARY VALUES(6, NULL, NULL);

-- 预期：NULL 值的距离为 NULL，排序时 NULL 值在最前面
SELECT ID, VEC3, L2_DISTANCE(VEC3, '[1,2,3]') AS DIST FROM T_VEC_BOUNDARY ORDER BY DIST;

-- ==================== 测试4: 维度不匹配 ====================
-- 测试：查询向量维度与字段维度不匹配
-- 预期：返回 NULL
SELECT ID, L2_DISTANCE(VEC3, '[1,2]') AS DIST FROM T_VEC_BOUNDARY WHERE ID = 1;
SELECT ID, L2_DISTANCE(VEC3, '[1,2,3,4]') AS DIST FROM T_VEC_BOUNDARY WHERE ID = 1;
SELECT ID, L2_DISTANCE(VEC10, '[1,2,3]') AS DIST FROM T_VEC_BOUNDARY WHERE ID = 1;

-- ==================== 测试5: 单行表 ====================
DELETE FROM T_VEC_BOUNDARY WHERE ID > 1;
SELECT * FROM T_VEC_BOUNDARY ORDER BY L2_DISTANCE(VEC3, '[1,2,3]') LIMIT 1;

-- ==================== 测试6: 用户报告的问题用例 ====================
-- 重建表模拟 T2
DROP TABLE IF EXISTS T2;
CREATE TABLE T2(C1 INT, C2 VECTOR(10), C3 VECTOR(10));

-- 插入测试数据
INSERT INTO T2 VALUES(1, '[1,2,3,4,5,6,7,8,9,10]', '[10,9,8,7,6,5,4,3,2,1]');
INSERT INTO T2 VALUES(2, '[8.2,9.13,4.37,9.68,3.09,8.04,7.61,9.58,4.32,3.49]', '[1,1,1,1,1,1,1,1,1,1]');
INSERT INTO T2 VALUES(3, '[5.5,6.6,7.7,8.8,9.9,1.1,2.2,3.3,4.4,5.5]', '[2,2,2,2,2,2,2,2,2,2]');

-- 原始查询（用户报告崩溃）
-- 预期：正常返回最近的向量
SELECT * FROM T2 ORDER BY L2_DISTANCE(C2, '[8.2,9.13,4.37,9.68,3.09,8.04,7.61,9.58,4.32,3.49]') LIMIT 1;

-- 测试：带距离输出
SELECT C1, L2_DISTANCE(C2, '[8.2,9.13,4.37,9.68,3.09,8.04,7.61,9.58,4.32,3.49]') AS DIST FROM T2 ORDER BY DIST LIMIT 1;

-- ==================== 测试7: 大数据集（如果性能允许）====================
DELETE FROM T2;
-- 插入多行数据
INSERT INTO T2 VALUES(1, '[1,2,3,4,5,6,7,8,9,10]', '[1,1,1,1,1,1,1,1,1,1]');
INSERT INTO T2 VALUES(2, '[2,3,4,5,6,7,8,9,10,11]', '[2,2,2,2,2,2,2,2,2,2]');
INSERT INTO T2 VALUES(3, '[3,4,5,6,7,8,9,10,11,12]', '[3,3,3,3,3,3,3,3,3,3]');
INSERT INTO T2 VALUES(4, '[4,5,6,7,8,9,10,11,12,13]', '[4,4,4,4,4,4,4,4,4,4]');
INSERT INTO T2 VALUES(5, '[5,6,7,8,9,10,11,12,13,14]', '[5,5,5,5,5,5,5,5,5,5]');

SELECT * FROM T2 ORDER BY L2_DISTANCE(C2, '[1,2,3,4,5,6,7,8,9,10]') LIMIT 3;

-- ==================== 测试8: 其他距离函数 ====================
-- COSINE_DISTANCE
SELECT C1, COSINE_DISTANCE(C2, '[1,2,3,4,5,6,7,8,9,10]') AS DIST FROM T2 ORDER BY DIST LIMIT 1;
-- DOT_PRODUCT
-- INNER_PRODUCT
SELECT C1, INNER_PRODUCT(C2, '[1,2,3,4,5,6,7,8,9,10]') AS DIST FROM T2 ORDER BY DIST DESC LIMIT 1;

-- ==================== 测试9: 零向量和极值 ====================
INSERT INTO T2 VALUES(100, '[0,0,0,0,0,0,0,0,0,0]', '[0,0,0,0,0,0,0,0,0,0]');
INSERT INTO T2 VALUES(101, '[999,999,999,999,999,999,999,999,999,999]', '[1,1,1,1,1,1,1,1,1,1]');
INSERT INTO T2 VALUES(102, '[-1,-2,-3,-4,-5,-6,-7,-8,-9,-10]', '[1,1,1,1,1,1,1,1,1,1]');

SELECT C1, L2_DISTANCE(C2, '[0,0,0,0,0,0,0,0,0,0]') AS DIST FROM T2 ORDER BY DIST LIMIT 3;

-- ==================== 清理 ====================
DROP TABLE T_VEC_BOUNDARY;
DROP TABLE T2;
