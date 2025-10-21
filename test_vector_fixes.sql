-- 向量数据库修复验证测试用例
-- 测试目标：验证维度不匹配、类型转换错误等问题的修复

-- 创建测试表
CREATE TABLE vector_test (
    id INT,
    vec1 VECTOR(3),
    vec2 VECTOR(4)
);

-- 插入测试数据
INSERT INTO vector_test VALUES
(1, STRING_TO_VECTOR('[1, 2, 3]'), STRING_TO_VECTOR('[1, 2, 3, 4]')),
(2, STRING_TO_VECTOR('[0.24, -9.97, 1.83]'), STRING_TO_VECTOR('[9.02, 3.51, 6.92, 1.5]')),
(3, STRING_TO_VECTOR('[2.32, -4.5, 5.77]'), STRING_TO_VECTOR('[5.37, 2.44, -6.31, 0.8]'));

-- =====================================================
-- 测试1：维度不匹配的向量距离计算
-- 修复前：L2_DISTANCE返回INVALID_ARGUMENT，DISTANCE返回NULL
-- 修复后：都应该返回NULL（一致处理）
-- =====================================================

-- 这应该返回NULL（维度不匹配：3维 vs 4维）
SELECT
    id,
    L2_DISTANCE(vec1, vec2) as l2_dist_mismatch,
    COSINE_DISTANCE(vec1, vec2) as cosine_dist_mismatch,
    INNER_PRODUCT(vec1, vec2) as inner_product_mismatch
FROM vector_test
WHERE id = 1;

-- 这也应该返回NULL（维度不匹配：3维 vs 4维）
SELECT
    id,
    DISTANCE(vec1, vec2, 'EUCLIDEAN') as dist_euc_mismatch,
    DISTANCE(vec1, vec2, 'COSINE') as dist_cos_mismatch,
    DISTANCE(vec1, vec2, 'DOT') as dist_dot_mismatch
FROM vector_test
WHERE id = 1;

-- =====================================================
-- 测试2：类型转换错误处理
-- 修复前：转换失败返回NULL
-- 修复后：应该返回FAILURE状态
-- =====================================================

-- 无效向量格式 - 应该返回FAILURE
SELECT
    DISTANCE(STRING_TO_VECTOR('[invalid, format]'),
             STRING_TO_VECTOR('[1, 2, 3]'),
             'EUCLIDEAN') as invalid_format_dist;

-- 不完整的向量格式 - 应该返回FAILURE
SELECT
    DISTANCE(STRING_TO_VECTOR('[1, 2,'),
             STRING_TO_VECTOR('[1, 2, 3]'),
             'EUCLIDEAN') as incomplete_format_dist;

-- =====================================================
-- 测试3：零向量处理测试
-- 验证余弦距离的零向量检测
-- =====================================================

-- 创建包含零向量的测试
CREATE TABLE zero_vector_test (
    id INT,
    vec VECTOR(3)
);

INSERT INTO zero_vector_test VALUES
(1, STRING_TO_VECTOR('[0, 0, 0]')),  -- 零向量
(2, STRING_TO_VECTOR('[1, 1, 1]')),  -- 非零向量
(3, STRING_TO_VECTOR('[0, 0, 1]'));  -- 部分零向量

-- 零向量余弦距离应该返回NULL
SELECT
    a.id as id1,
    b.id as id2,
    COSINE_DISTANCE(a.vec, b.vec) as cosine_dist,
    DISTANCE(a.vec, b.vec, 'COSINE') as cosine_dist_func
FROM zero_vector_test a, zero_vector_test b
WHERE a.id = 1 AND b.id > 1;  -- 零向量与非零向量

-- =====================================================
-- 测试4：正常向量距离计算验证
-- 确保修复没有破坏正常功能
-- =====================================================

-- 相同向量距离应该为0
SELECT
    DISTANCE(STRING_TO_VECTOR('[1, 2, 3]'),
             STRING_TO_VECTOR('[1, 2, 3]'),
             'EUCLIDEAN') as same_vector_euc,
    DISTANCE(STRING_TO_VECTOR('[1, 2, 3]'),
             STRING_TO_VECTOR('[1, 2, 3]'),
             'COSINE') as same_vector_cos,
    DISTANCE(STRING_TO_VECTOR('[1, 2, 3]'),
             STRING_TO_VECTOR('[1, 2, 3]'),
             'DOT') as same_vector_dot;

-- 不同向量的距离计算
SELECT
    DISTANCE(STRING_TO_VECTOR('[7.32, -7.41, -5.2, 5.07, 0.32]'),
             STRING_TO_VECTOR('[6.08, 0.84, -4.51, 0.67, -3.35]'),
             'COSINE') as dist_cosine,

    DISTANCE(STRING_TO_VECTOR('[-9.01, 3.01, 7.85, -0.09, -9.76]'),
             STRING_TO_VECTOR('[-7.09, -8.48, -9.13, 8.14, 7.56]'),
             'EUCLIDEAN') as dist_euclidean,

    DISTANCE(STRING_TO_VECTOR('[-2.26, -3.05, -3.8, -4.64, 3.72]'),
             STRING_TO_VECTOR('[0.6, -2.08, 6.14, 0.59, -6.47]'),
             'DOT') as dist_dot;

-- =====================================================
-- 测试5：VECTOR_TO_STRING函数测试
-- 验证类型转换修复
-- =====================================================

-- 正常向量转字符串
SELECT id, VECTOR_TO_STRING(vec1) as vec_str FROM vector_test WHERE id = 1;

-- 无效向量转字符串 - 应该返回FAILURE
SELECT VECTOR_TO_STRING(STRING_TO_VECTOR('[invalid]')) as invalid_vec_str;

-- =====================================================
-- 测试6：数值稳定性测试
-- 测试余弦距离边界保护
-- =====================================================

-- 测试非常接近但数值上可能导致精度问题的向量
SELECT
    DISTANCE(STRING_TO_VECTOR('[1e-10, 1e-10, 1e-10]'),
             STRING_TO_VECTOR('[1e-10, 1e-10, 2e-10]'),
             'COSINE') as precision_test;

-- 清理测试数据
DROP TABLE vector_test;
DROP TABLE zero_vector_test;