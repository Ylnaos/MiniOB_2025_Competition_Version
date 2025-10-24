-- 复杂子查询修复验证测试 - 新版本
-- 测试OR操作和聚合函数子查询的修复效果

-- 创建新的测试表
CREATE TABLE employees(id int, name char(20), age int, salary float, dept_id int);
CREATE TABLE departments(id int, dept_name char(20), budget float);
CREATE TABLE projects(id int, project_name char(20), cost float, emp_id int);
CREATE TABLE bonuses(id int, emp_id int, bonus_amount float, year int);

-- 插入测试数据
INSERT INTO employees VALUES (1, 'Alice', 25, 5000.0, 10);
INSERT INTO employees VALUES (2, 'Bob', 30, 6000.0, 20);
INSERT INTO employees VALUES (3, 'Carol', 28, 5500.0, 10);
INSERT INTO employees VALUES (4, 'David', 35, 7000.0, 30);
INSERT INTO employees VALUES (5, 'Eve', 22, 4500.0, 20);
INSERT INTO employees VALUES (6, 'Frank', 40, 8000.0, 10);

INSERT INTO departments VALUES (10, 'Engineering', 100000.0);
INSERT INTO departments VALUES (20, 'Marketing', 80000.0);
INSERT INTO departments VALUES (30, 'Sales', 120000.0);

INSERT INTO projects VALUES (101, 'Project A', 50000.0, 1);
INSERT INTO projects VALUES (102, 'Project B', 30000.0, 2);
INSERT INTO projects VALUES (103, 'Project C', 40000.0, 3);
INSERT INTO projects VALUES (104, 'Project D', 60000.0, 4);
INSERT INTO projects VALUES (105, 'Project E', 25000.0, 5);

INSERT INTO bonuses VALUES (1001, 1, 1000.0, 2023);
INSERT INTO bonuses VALUES (1002, 2, 1500.0, 2023);
INSERT INTO bonuses VALUES (1003, 3, 800.0, 2023);
INSERT INTO bonuses VALUES (1004, 4, 2000.0, 2023);
INSERT INTO bonuses VALUES (1005, 5, 500.0, 2023);
INSERT INTO bonuses VALUES (1006, 6, 2500.0, 2023);

-- 测试1：基础复杂OR子查询（用户提到的类似场景）
-- 查找年龄大于最小薪资或薪资小于最大奖金的员工
SELECT * FROM employees WHERE age > (SELECT min(salary) FROM employees) OR salary < (SELECT max(bonus_amount) FROM bonuses);

-- 测试2：带部门条件的复杂子查询
-- 查找薪资大于平均薪资或者所在部门预算大于100000的员工
SELECT * FROM employees WHERE salary > (SELECT avg(salary) FROM employees) OR dept_id IN (SELECT id FROM departments WHERE budget > 100000);

-- 测试3：EXISTS子查询
-- 查找有项目管理的员工
SELECT * FROM employees e WHERE EXISTS (SELECT * FROM projects p WHERE p.emp_id = e.id AND p.cost > 40000);

-- 测试4：NOT EXISTS子查询
-- 查找没有奖金的员工
SELECT * FROM employees e WHERE NOT EXISTS (SELECT * FROM bonuses b WHERE b.emp_id = e.id);

-- 测试5：IN子查询
-- 查找工程部门的员工
SELECT * FROM employees WHERE dept_id IN (SELECT id FROM departments WHERE dept_name = 'Engineering');

-- 测试6：NOT IN子查询
-- 查找不在工程部门的员工
SELECT * FROM employees WHERE dept_id NOT IN (SELECT id FROM departments WHERE dept_name = 'Engineering');

-- 测试7：嵌套子查询
-- 查找薪资大于有项目员工平均薪资的员工
SELECT * FROM employees WHERE salary > (SELECT avg(salary) FROM employees WHERE id IN (SELECT emp_id FROM projects));

-- 测试8：复合条件子查询（OR + AND + 子查询）
-- 查找（年龄大于30且薪资大于平均薪资）或者有高奖金的员工
SELECT * FROM employees WHERE (age > 30 AND salary > (SELECT avg(salary) FROM employees)) OR id IN (SELECT emp_id FROM bonuses WHERE bonus_amount > 1500);

-- 测试9：多层级嵌套子查询
-- 查找薪资大于工程部门平均薪资的员工
SELECT * FROM employees WHERE salary > (SELECT avg(e.salary) FROM employees e WHERE e.dept_id IN (SELECT d.id FROM departments d WHERE d.dept_name = 'Engineering'));

-- 测试10：复杂条件组合
-- 查找满足以下任一条件的员工：年龄大于平均年龄、薪资大于平均薪资、或者有高价值项目
SELECT * FROM employees WHERE age > (SELECT avg(age) FROM employees) OR salary > (SELECT avg(salary) FROM employees) OR id IN (SELECT emp_id FROM projects WHERE cost > 45000);

-- 清理测试表
DROP TABLE employees;
DROP TABLE departments;
DROP TABLE projects;
DROP TABLE bonuses;