-- 维度不匹配修复前后对比测试
-- 显示修复前后的预期行为变化

-- 测试场景：4维向量 vs 5维向量
SELECT '=== 4维 vs 5维向量测试 ===' as test_case;

-- DISTANCE函数
SELECT 'DISTANCE函数 - 修复前:NULL, 修复后:FAILURE' as description;
SELECT DISTANCE(STRING_TO_VECTOR('[1.77,3.59,3.97,-7.9]'),
               STRING_TO_VECTOR('[8.94,-7.18,-1.87,8.58,2.08]'),
               'DOT') AS result;

-- L2_DISTANCE函数
SELECT 'L2_DISTANCE函数 - 修复前:NULL, 修复后:FAILURE' as description;
SELECT L2_DISTANCE(STRING_TO_VECTOR('[1,2,3,4]'),
                  STRING_TO_VECTOR('[1,2,3]')) AS result;

-- COSINE_DISTANCE函数
SELECT 'COSINE_DISTANCE函数 - 修复前:NULL, 修复后:FAILURE' as description;
SELECT COSINE_DISTANCE(STRING_TO_VECTOR('[1,2,3,4]'),
                      STRING_TO_VECTOR('[1,2,3]')) AS result;

-- INNER_PRODUCT函数
SELECT 'INNER_PRODUCT函数 - 修复前:NULL, 修复后:FAILURE' as description;
SELECT INNER_PRODUCT(STRING_TO_VECTOR('[1,2,3,4]'),
                    STRING_TO_VECTOR('[1,2,3]')) AS result;

SELECT '=== 正常维度测试（应该保持不变）===' as test_case;

-- 相同维度的正常计算
SELECT 'DISTANCE正常计算 - 应该返回数值' as description;
SELECT DISTANCE(STRING_TO_VECTOR('[1,2,3]'),
               STRING_TO_VECTOR('[4,5,6]'),
               'EUCLIDEAN') AS result;

SELECT 'L2_DISTANCE正常计算 - 应该返回数值' as description;
SELECT L2_DISTANCE(STRING_TO_VECTOR('[1,2,3]'),
                  STRING_TO_VECTOR('[4,5,6]')) AS result;