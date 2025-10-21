-- 快速验证维度不匹配修复
-- 只测试最关键的几个案例

-- 关键测试：原测试失败案例4（4维vs5维）
SELECT 'TEST 1: 4维 vs 5维向量 DOT距离' as test_name;
SELECT DISTANCE(STRING_TO_VECTOR('[1.77,3.59,3.97,-7.9]'),
               STRING_TO_VECTOR('[8.94,-7.18,-1.87,8.58,2.08]'),
               'DOT') AS result;

-- 关键测试：3维vs4维
SELECT 'TEST 2: 3维 vs 4维向量 COSINE距离' as test_name;
SELECT DISTANCE(STRING_TO_VECTOR('[1,2,3]'),
               STRING_TO_VECTOR('[1,2,3,4]'),
               'COSINE') AS result;

-- 关键测试：L2_DISTANCE维度不匹配
SELECT 'TEST 3: L2_DISTANCE 2维vs3维' as test_name;
SELECT L2_DISTANCE(STRING_TO_VECTOR('[1,2]'),
                  STRING_TO_VECTOR('[1,2,3]')) AS result;

-- 正常测试（确保没破坏功能）
SELECT 'TEST 4: 正常相同维度计算' as test_name;
SELECT DISTANCE(STRING_TO_VECTOR('[1,2,3]'),
               STRING_TO_VECTOR('[4,5,6]'),
               'EUCLIDEAN') AS result;