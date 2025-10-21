-- 维度不匹配修复验证测试
-- 测试目标：验证所有维度不匹配的向量操作都返回FAILURE

-- =====================================================
-- 测试1：DISTANCE函数维度不匹配测试
-- 修复前：返回NULL
-- 修复后：应该返回FAILURE
-- =====================================================

-- 3维 vs 4维向量 - COSINE距离
SELECT DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3,4]'), 'COSINE') AS test1;

-- 3维 vs 4维向量 - EUCLIDEAN距离
SELECT DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3,4]'), 'EUCLIDEAN') AS test2;

-- 3维 vs 4维向量 - DOT距离
SELECT DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3,4]'), 'DOT') AS test3;

-- 4维 vs 5维向量 - DOT距离（来自原测试失败案例）
SELECT DISTANCE(STRING_TO_VECTOR('[1.77,3.59,3.97,-7.9]'), STRING_TO_VECTOR('[8.94,-7.18,-1.87,8.58,2.08]'), 'DOT') AS test4;

-- 5维 vs 4维向量 - EUCLIDEAN距离
SELECT DISTANCE(STRING_TO_VECTOR('[1.39,-8.57,3.35,9.7,-9.75]'), STRING_TO_VECTOR('[-4.48,6.93,6.04,-9.85]'), 'EUCLIDEAN') AS test5;

-- =====================================================
-- 测试2：L2_DISTANCE函数维度不匹配测试
-- 修复前：返回NULL
-- 修复后：应该返回FAILURE
-- =====================================================

-- 3维 vs 4维向量
SELECT L2_DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3,4]')) AS l2_test1;

-- 5维 vs 3维向量
SELECT L2_DISTANCE(STRING_TO_VECTOR('[1.39,-8.57,3.35,9.7,-9.75]'), STRING_TO_VECTOR('[1,2,3]')) AS l2_test2;

-- =====================================================
-- 测试3：COSINE_DISTANCE函数维度不匹配测试
-- 修复前：返回NULL
-- 修复后：应该返回FAILURE
-- =====================================================

-- 3维 vs 4维向量
SELECT COSINE_DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3,4]')) AS cos_test1;

-- 4维 vs 5维向量
SELECT COSINE_DISTANCE(STRING_TO_VECTOR('[1.77,3.59,3.97,-7.9]'), STRING_TO_VECTOR('[8.94,-7.18,-1.87,8.58,2.08]')) AS cos_test2;

-- =====================================================
-- 测试4：INNER_PRODUCT函数维度不匹配测试
-- 修复前：返回NULL
-- 修复后：应该返回FAILURE
-- =====================================================

-- 3维 vs 4维向量
SELECT INNER_PRODUCT(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3,4]')) AS inner_test1;

-- 2维 vs 3维向量
SELECT INNER_PRODUCT(STRING_TO_VECTOR('[0.03,-6.6]'), STRING_TO_VECTOR('[5.78,3.2,2.78]')) AS inner_test2;

-- =====================================================
-- 测试5：正常维度匹配的向量计算（应该成功）
-- 验证修复没有破坏正常功能
-- =====================================================

-- 相同维度向量的DISTANCE计算
SELECT DISTANCE(STRING_TO_VECTOR('[1.39,-8.57,3.35,9.7,-9.75]'),
               STRING_TO_VECTOR('[-4.48,6.93,6.04,-9.85,-2.92]'),
               'COSINE') AS normal_test1;

SELECT DISTANCE(STRING_TO_VECTOR('[-1.77,-9.38,0.54,1.99,3.18]'),
               STRING_TO_VECTOR('[-7.37,-8.36,0.85,5.78,1.86]'),
               'EUCLIDEAN') AS normal_test2;

SELECT DISTANCE(STRING_TO_VECTOR('[0.03,-6.6,8.35,3.48,-5.52]'),
               STRING_TO_VECTOR('[5.78,3.2,2.78,9.33,6.35]'),
               'DOT') AS normal_test3;

-- 相同维度向量的L2_DISTANCE计算
SELECT L2_DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[4,5,6]')) AS normal_l2;

-- 相同维度向量的COSINE_DISTANCE计算
SELECT COSINE_DISTANCE(STRING_TO_VECTOR('[1,0,0]'), STRING_TO_VECTOR('[0,1,0]')) AS normal_cosine;

-- 相同维度向量的INNER_PRODUCT计算
SELECT INNER_PRODUCT(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[4,5,6]')) AS normal_inner;

-- =====================================================
-- 测试6：边界情况测试
-- =====================================================

-- 空向量 vs 正常向量
SELECT DISTANCE(STRING_TO_VECTOR('[]'), STRING_TO_VECTOR('[1,2,3]'), 'EUCLIDEAN') AS empty_test1;

-- 单维向量 vs 多维向量
SELECT DISTANCE(STRING_TO_VECTOR('[1]'), STRING_TO_VECTOR('[1,2]'), 'DOT') AS single_dim_test;

-- 大维度差异
SELECT DISTANCE(STRING_TO_VECTOR('[1,2]'), STRING_TO_VECTOR('[1,2,3,4,5,6,7,8,9,10]'), 'COSINE') AS large_dim_test;