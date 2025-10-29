-- =============================================================================
-- IF NOT EXISTS / IF EXISTS 功能测试用例
-- =============================================================================

-- 清理环境
DROP TABLE IF EXISTS test_table;
DROP VIEW IF EXISTS test_view;

-- =============================================================================
-- 测试 1: CREATE TABLE IF NOT EXISTS
-- =============================================================================
SHOW TABLES;

-- 1.1 创建表（应该成功）
CREATE TABLE test_table (
    id INT,
    name VARCHAR(50),
    age INT
);

-- 1.2 再次创建同名表（不使用 IF NOT EXISTS，应该失败）
CREATE TABLE test_table (
    id INT,
    name VARCHAR(50)
);

-- 1.3 使用 IF NOT EXISTS 创建同名表（应该成功，不报错）
CREATE TABLE IF NOT EXISTS test_table (
    id INT,
    name VARCHAR(50)
);

-- 1.4 验证表确实存在
DESC test_table;

-- =============================================================================
-- 测试 2: DROP TABLE IF EXISTS
-- =============================================================================

-- 2.1 删除存在的表（应该成功）
DROP TABLE IF EXISTS test_table;

-- 2.2 删除已经不存在的表（不使用 IF EXISTS，应该失败）
DROP TABLE test_table;

-- 2.3 使用 IF EXISTS 删除不存在的表（应该成功，不报错）
DROP TABLE IF EXISTS test_table;

-- 2.4 重新创建表用于后续测试
CREATE TABLE test_table (
    id INT,
    name VARCHAR(50),
    age INT
);

-- =============================================================================
-- 测试 3: DROP INDEX IF EXISTS
-- =============================================================================

-- 3.1 在表上创建索引
CREATE INDEX idx_name ON test_table (name);

-- 3.2 删除存在的索引（应该成功）
DROP INDEX IF EXISTS idx_name ON test_table;

-- 3.3 删除已经不存在的索引（不使用 IF EXISTS，应该失败）
DROP INDEX idx_not_exist ON test_table;

-- 3.4 使用 IF EXISTS 删除不存在的索引（应该成功，不报错）
DROP INDEX IF EXISTS idx_not_exist ON test_table;

-- 3.5 在不存在的表上删除索引（应该成功，不报错）
DROP INDEX IF EXISTS idx_name ON table_not_exist;

-- =============================================================================
-- 测试 4: DROP VIEW IF EXISTS
-- =============================================================================

-- 4.1 创建视图
CREATE VIEW test_view AS SELECT id, name FROM test_table;

-- 4.2 删除存在的视图（应该成功）
DROP VIEW IF EXISTS test_view;

-- 4.3 删除已经不存在的视图（不使用 IF EXISTS，应该失败）
DROP VIEW view_not_exist;

-- 4.4 使用 IF EXISTS 删除不存在的视图（应该成功，不报错）
DROP VIEW IF EXISTS view_not_exist;

-- =============================================================================
-- 测试 5: 组合测试 - 幂等性
-- =============================================================================

-- 5.1 重复执行创建和删除（应该都成功）
CREATE TABLE IF NOT EXISTS test_table2 (id INT, value VARCHAR(100));
CREATE TABLE IF NOT EXISTS test_table2 (id INT, value VARCHAR(100));
CREATE TABLE IF NOT EXISTS test_table2 (id INT, value VARCHAR(100));

DROP TABLE IF EXISTS test_table2;
DROP TABLE IF EXISTS test_table2;
DROP TABLE IF EXISTS test_table2;

-- 5.2 创建索引的幂等性
CREATE TABLE test_table3 (id INT, name VARCHAR(50));
CREATE INDEX IF NOT EXISTS idx_test ON test_table3 (id);
CREATE INDEX IF NOT EXISTS idx_test ON test_table3 (id);
CREATE INDEX IF NOT EXISTS idx_test ON test_table3 (id);

-- 清理
DROP INDEX IF EXISTS idx_test ON test_table3;
DROP TABLE IF EXISTS test_table3;

-- =============================================================================
-- 测试 6: CREATE TABLE ... AS SELECT 与 IF NOT EXISTS
-- =============================================================================

-- 6.1 创建源表并插入数据
CREATE TABLE source_table (id INT, value VARCHAR(50));
INSERT INTO source_table VALUES (1, 'test1');
INSERT INTO source_table VALUES (2, 'test2');

-- 6.2 使用 CTAS 创建表
CREATE TABLE IF NOT EXISTS target_table AS SELECT * FROM source_table;

-- 6.3 再次执行（应该成功，不报错）
CREATE TABLE IF NOT EXISTS target_table AS SELECT * FROM source_table;

-- 6.4 验证数据
SELECT * FROM target_table;

-- 清理
DROP TABLE IF EXISTS target_table;
DROP TABLE IF EXISTS source_table;

-- =============================================================================
-- 最终清理
-- =============================================================================
DROP TABLE IF EXISTS test_table;
DROP VIEW IF EXISTS test_view;

SHOW TABLES;

-- =============================================================================
-- 测试完成
-- =============================================================================
