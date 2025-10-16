-- U87快速测试：复现官方CREATE VIEW失败场景

-- 初始化
DROP TABLE IF EXISTS create_view_t1;
DROP TABLE IF EXISTS create_view_t2;
DROP VIEW IF EXISTS create_view_v7;

CREATE TABLE create_view_t1(id INT, age INT, name CHAR(10));
CREATE TABLE create_view_t2(id INT, age INT, name CHAR(10));

-- 插入测试数据
INSERT INTO create_view_t1 VALUES(1, 20, 'Alice');
INSERT INTO create_view_t1 VALUES(2, 25, 'Bob');
INSERT INTO create_view_t1 VALUES(3, 30, 'Charlie');

-- 官方失败场景：创建自连接聚合视图
CREATE VIEW create_view_v7 AS
SELECT COUNT(t1.id) AS num, SUM(t1.age)+SUM(t2.age) AS data
FROM create_view_t1 t1, create_view_t1 t2
WHERE t1.id=t2.id;

-- 关键测试：查询视图的COUNT（官方预期返回1）
SELECT COUNT(*) FROM create_view_v7;

-- 额外验证：查看视图内容
SELECT * FROM create_view_v7;
