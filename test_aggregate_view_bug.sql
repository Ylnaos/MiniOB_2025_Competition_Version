-- 测试聚合视图bug的完整用例

-- 清理
DROP TABLE IF EXISTS test_agg_t;
DROP VIEW IF EXISTS test_agg_v;

-- 创建测试表并插入数据
CREATE TABLE test_agg_t(id INT, age INT, name CHAR(10));

-- 插入100条测试数据（模拟原问题场景）
INSERT INTO test_agg_t VALUES (1, 20, 'a');
INSERT INTO test_agg_t VALUES (2, 21, 'b');
INSERT INTO test_agg_t VALUES (3, 22, 'c');
INSERT INTO test_agg_t VALUES (4, 23, 'd');
INSERT INTO test_agg_t VALUES (5, 24, 'e');
INSERT INTO test_agg_t VALUES (6, 25, 'f');
INSERT INTO test_agg_t VALUES (7, 26, 'g');
INSERT INTO test_agg_t VALUES (8, 27, 'h');
INSERT INTO test_agg_t VALUES (9, 28, 'i');
INSERT INTO test_agg_t VALUES (10, 29, 'j');

-- 创建聚合视图（自连接）
-- 视图应该返回1行：num=10（匹配的行数），data=sum(age)*2
CREATE VIEW test_agg_v AS SELECT COUNT(t1.id) AS num, SUM(t1.age) + SUM(t2.age) AS data FROM test_agg_t t1, test_agg_t t2 WHERE t1.id = t2.id;

-- 查看视图内容（应该返回1行）
SELECT * FROM test_agg_v;

-- 对视图结果做聚合
-- 期望：SUM(num) = 10（因为视图返回1行，num=10）
-- 错误：如果内层没有聚合，会返回 10*10=100（对10行分别聚合）
SELECT SUM(num) FROM test_agg_v;

-- 期望：COUNT(*) = 1（视图返回1行）
-- 错误：如果内层没有聚合，会返回 10（视图返回10行）
SELECT COUNT(*) FROM test_agg_v;
