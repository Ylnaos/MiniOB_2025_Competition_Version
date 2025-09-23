-- 算术表达式测试用例

-- 创建测试表
CREATE TABLE exp_test(
  id INT,
  a INT,
  b INT,
  c FLOAT,
  d FLOAT
);

-- 插入测试数据
INSERT INTO exp_test VALUES (1, 10, 5, 3.5, 2.0);
INSERT INTO exp_test VALUES (2, 20, 0, 4.5, 0.0);
INSERT INTO exp_test VALUES (3, -5, 10, -2.5, 3.0);
INSERT INTO exp_test VALUES (4, 8, 4, 2.0, 4.0);
INSERT INTO exp_test VALUES (5, 15, 3, 5.5, 1.5);

-- 1. 基本算术运算测试

-- 加法
SELECT id, a + b FROM exp_test;
SELECT id, c + d FROM exp_test;
SELECT id, a + c FROM exp_test;
SELECT id, 10 + 20 FROM exp_test WHERE id = 1;

-- 减法
SELECT id, a - b FROM exp_test;
SELECT id, c - d FROM exp_test;
SELECT id, b - a FROM exp_test;
SELECT id, 100 - 25 FROM exp_test WHERE id = 1;

-- 乘法
SELECT id, a * b FROM exp_test;
SELECT id, c * d FROM exp_test;
SELECT id, a * c FROM exp_test;
SELECT id, 5 * 6 FROM exp_test WHERE id = 1;

-- 除法（整数除法返回浮点数）
SELECT id, a / b FROM exp_test WHERE b != 0;
SELECT id, c / d FROM exp_test WHERE d > 0.001 OR d < -0.001;
SELECT id, a / 2 FROM exp_test;
SELECT id, 100 / 4 FROM exp_test WHERE id = 1;

-- 负号
SELECT id, -a FROM exp_test;
SELECT id, -c FROM exp_test;
SELECT id, -(a + b) FROM exp_test;

-- 2. 复杂表达式测试

-- 混合运算（运算优先级）
SELECT id, a + b * c FROM exp_test;
SELECT id, (a + b) * c FROM exp_test;
SELECT id, a * b + c * d FROM exp_test;
SELECT id, a / b - c / d FROM exp_test WHERE b != 0 AND d != 0;

-- 嵌套表达式
SELECT id, (a + b) * (c - d) FROM exp_test;
SELECT id, a * (b + c * d) FROM exp_test;
SELECT id, ((a + b) * c) / d FROM exp_test WHERE d != 0;

-- 多层嵌套
SELECT id, -(a * (-b + c)) + (d * 2) FROM exp_test;
SELECT id, ((a + 5) * (b - 2)) / (c + d) FROM exp_test WHERE (c + d) != 0;

-- 3. WHERE条件中的表达式

-- 算术比较
SELECT * FROM exp_test WHERE a + b > 15;
SELECT * FROM exp_test WHERE a * 2 < b * 5;
SELECT * FROM exp_test WHERE c - d > 1.0;
SELECT * FROM exp_test WHERE a / 2 = b;

-- 复杂条件
SELECT * FROM exp_test WHERE (a + b) * c > 50;
SELECT * FROM exp_test WHERE a * b + c * d < 30;
SELECT * FROM exp_test WHERE (a - b) * (c + d) != 0;

-- 4. 除零处理测试

-- 整数除零（应返回NULL）
SELECT id, a / b FROM exp_test WHERE b = 0;
SELECT id, 10 / 0 FROM exp_test WHERE id = 1;
SELECT id, a / (b - b) FROM exp_test WHERE id = 1;

-- 浮点数除零（应返回NULL）
SELECT id, c / d FROM exp_test WHERE d = 0.0;
SELECT id, 3.14 / 0.0 FROM exp_test WHERE id = 1;
SELECT id, c / (d - d) FROM exp_test WHERE id = 1;

-- 除零在表达式中（应传播NULL）
SELECT id, (a + b) / 0 FROM exp_test WHERE id = 1;
SELECT id, a + b / 0 FROM exp_test WHERE b = 0;
SELECT id, (a / 0) + b FROM exp_test WHERE b = 0;

-- NULL值比较（应返回false）
SELECT * FROM exp_test WHERE a / 0 = a / 0;
SELECT * FROM exp_test WHERE a / 0 > 10;
SELECT * FROM exp_test WHERE a / 0 < 10;
SELECT * FROM exp_test WHERE a / 0 != a / 0;

-- 5. 类型转换测试

-- 整数与浮点数混合运算
SELECT id, a + c FROM exp_test;
SELECT id, b * d FROM exp_test;
SELECT id, a / c FROM exp_test WHERE c != 0;

-- 整数运算结果类型
SELECT id, a + b, (a + b) * 1.0 FROM exp_test;
SELECT id, a * b, (a * b) / 1.0 FROM exp_test;

-- 6. 边界值测试

-- 大数值运算
SELECT 2147483647 + 0;
SELECT 2147483647 - 1;
SELECT 1000000 * 1000;

-- 小数精度
SELECT 0.1 + 0.2;
SELECT 1.0 / 3.0;
SELECT 3.14159 * 2.0;

-- 负数运算
SELECT -10 + 5;
SELECT -10 - (-5);
SELECT -10 * (-5);
SELECT -10 / (-2);

-- 7. 多表连接中的表达式

CREATE TABLE exp_test2(
  id INT,
  x INT,
  y FLOAT
);

INSERT INTO exp_test2 VALUES (1, 100, 10.0);
INSERT INTO exp_test2 VALUES (2, 200, 20.0);
INSERT INTO exp_test2 VALUES (3, 300, 30.0);

-- 跨表算术运算
SELECT t1.id, t1.a + t2.x, t1.c * t2.y
FROM exp_test t1, exp_test2 t2
WHERE t1.id = t2.id;

-- 跨表复杂表达式
SELECT t1.id, (t1.a + t1.b) * t2.x / t2.y
FROM exp_test t1, exp_test2 t2
WHERE t1.id = t2.id AND t2.y != 0;

-- WHERE条件中的跨表表达式
SELECT t1.*, t2.*
FROM exp_test t1, exp_test2 t2
WHERE t1.a * 10 = t2.x;

-- 清理
DROP TABLE exp_test;
DROP TABLE exp_test2;