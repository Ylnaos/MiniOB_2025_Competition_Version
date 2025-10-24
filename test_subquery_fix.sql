-- 复杂子查询修复验证测试
-- 测试用户提到的具体问题场景

-- 创建新测试表
CREATE TABLE l1(id int, age int, score float, name char(20));
CREATE TABLE l2(id int, dept_id int, salary float);
CREATE TABLE l3(id int, level int, bonus float);

-- 插入测试数据
INSERT INTO l1 VALUES (1, 25, 85.5, 'Alice');
INSERT INTO l1 VALUES (2, 30, 92.0, 'Bob');
INSERT INTO l1 VALUES (3, 28, 78.5, 'Carol');
INSERT INTO l1 VALUES (4, 35, 88.0, 'David');
INSERT INTO l1 VALUES (5, 22, 76.5, 'Eve');

INSERT INTO l2 VALUES (1, 10, 5000.0);
INSERT INTO l2 VALUES (2, 20, 6000.0);
INSERT INTO l2 VALUES (3, 10, 4500.0);
INSERT INTO l2 VALUES (4, 30, 7000.0);
INSERT INTO l2 VALUES (5, 20, 5500.0);

INSERT INTO l3 VALUES (1, 1, 500.0);
INSERT INTO l3 VALUES (2, 2, 800.0);
INSERT INTO l3 VALUES (3, 1, 400.0);
INSERT INTO l3 VALUES (4, 3, 1000.0);
INSERT INTO l3 VALUES (5, 2, 600.0);

-- 测试1：基础复杂子查询 - 用户提到的问题场景
-- 预期结果：age > (select min(salary) from l2) OR score <= (select min(bonus) from l3)
SELECT * FROM l1 WHERE age > (SELECT min(salary) FROM l2) OR score <= (SELECT min(bonus) FROM l3);

-- 测试2：带聚合函数的复杂子查询
SELECT * FROM l1 WHERE age > (SELECT avg(salary) FROM l2);

-- 测试3：EXISTS子查询
SELECT * FROM l1 WHERE EXISTS (SELECT * FROM l2 WHERE l2.id = l1.id AND l2.salary > 5500);

-- 测试4：NOT EXISTS子查询
SELECT * FROM l1 WHERE NOT EXISTS (SELECT * FROM l2 WHERE l2.dept_id = 99);

-- 测试5：IN子查询
SELECT * FROM l1 WHERE id IN (SELECT id FROM l2 WHERE salary > 5000);

-- 测试6：NOT IN子查询
SELECT * FROM l1 WHERE id NOT IN (SELECT id FROM l3 WHERE level = 1);

-- 测试7：嵌套子查询
SELECT * FROM l1 WHERE age > (SELECT max(salary) FROM l2 WHERE id IN (SELECT id FROM l3 WHERE bonus > 500));

-- 测试8：复合条件子查询
SELECT * FROM l1 WHERE (age > (SELECT min(salary) FROM l2) AND score > 80) OR (id IN (SELECT id FROM l3 WHERE level > 1));

-- 清理测试表
DROP TABLE l1;
DROP TABLE l2;
DROP TABLE l3;