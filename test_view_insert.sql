-- 测试多表视图INSERT功能修复
-- 根据用户提供的测试用例进行验证

-- 清理环境
DROP TABLE IF EXISTS create_view_t17;
DROP TABLE IF EXISTS create_view_t27;
DROP VIEW IF EXISTS create_view_v47;

-- 创建基础表
CREATE TABLE create_view_t17(id int, age int, name char(10));
CREATE TABLE create_view_t27(id int, age int, name char(10));

-- 创建多表视图
CREATE VIEW create_view_v47 AS select t17.id AS id, t17.age AS age, t27.name AS name FROM create_view_t17 t17, create_view_t27 t27 WHERE t17.id = t27.id;

-- 测试1: 插入完整数据 (应该成功)
INSERT INTO create_view_v47 VALUES(23, 23, 'JBNEMH');

-- 测试2: 插入部分数据 (应该成功)
INSERT INTO create_view_v47(id, age) VALUES(42, 42);

-- 验证插入结果
SELECT * FROM create_view_t17 ORDER BY id;
SELECT * FROM create_view_t27 ORDER BY id;

-- 清理环境
DROP VIEW IF EXISTS create_view_v47;
DROP TABLE IF EXISTS create_view_t17;
DROP TABLE IF EXISTS create_view_t27;