-- MiniOB UPDATE 功能测试用例
-- 测试单字段更新功能

-- 创建测试表
CREATE TABLE test_update(id int, name char, age int, score float);

-- 插入测试数据
INSERT INTO test_update VALUES(1, 'Alice', 20, 85.5);
INSERT INTO test_update VALUES(2, 'Bob', 25, 90.0);
INSERT INTO test_update VALUES(3, 'Charlie', 30, 75.5);
INSERT INTO test_update VALUES(4, 'David', 35, 88.0);
INSERT INTO test_update VALUES(5, 'Eva', 28, 92.5);

-- 查看初始数据
SELECT * FROM test_update;

-- 测试1：不带条件的更新（更新所有记录）
UPDATE test_update SET age=40;
SELECT * FROM test_update;

-- 测试2：带简单条件的更新
UPDATE test_update SET age=25 WHERE id=1;
SELECT * FROM test_update WHERE id=1;

-- 测试3：更新字符串字段
UPDATE test_update SET name='Frank' WHERE id=2;
SELECT * FROM test_update WHERE id=2;

-- 测试4：更新浮点数字段
UPDATE test_update SET score=95.5 WHERE id=3;
SELECT * FROM test_update WHERE id=3;

-- 测试5：带多个条件的更新
UPDATE test_update SET age=30 WHERE id=4 AND score=88.0;
SELECT * FROM test_update WHERE id=4;

-- 测试6：更新不存在的记录（应该不影响任何记录）
UPDATE test_update SET age=50 WHERE id=100;
SELECT * FROM test_update;

-- 创建带索引的表进行索引更新测试
CREATE TABLE test_update_index(id int, name char, age int);
CREATE INDEX idx_id ON test_update_index(id);
CREATE INDEX idx_age ON test_update_index(age);

-- 插入测试数据
INSERT INTO test_update_index VALUES(1, 'Alice', 20);
INSERT INTO test_update_index VALUES(2, 'Bob', 25);
INSERT INTO test_update_index VALUES(3, 'Charlie', 30);

-- 查看初始数据
SELECT * FROM test_update_index;

-- 测试7：更新带索引的字段
UPDATE test_update_index SET id=10 WHERE id=1;
SELECT * FROM test_update_index WHERE id=10;

-- 测试8：更新另一个带索引的字段
UPDATE test_update_index SET age=35 WHERE id=2;
SELECT * FROM test_update_index WHERE age=35;

-- 测试9：同时更新记录（测试索引一致性）
UPDATE test_update_index SET age=40 WHERE age=30;
SELECT * FROM test_update_index WHERE age=40;

-- 清理测试数据
DROP TABLE test_update;
DROP TABLE test_update_index;