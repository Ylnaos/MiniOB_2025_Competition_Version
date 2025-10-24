-- 测试复杂子查询功能修复
-- 包含用户提到的问题场景

-- 创建测试表
CREATE TABLE csq_1(id int, col1 int, feat1 float);
CREATE TABLE csq_2(id int, col2 int, feat2 float);
CREATE TABLE csq_3(id int, col3 int, feat3 float);
CREATE TABLE csq_4(id int, col4 int, feat4 float);

-- 插入测试数据
INSERT INTO csq_1 VALUES (1, 2, 16.52);
INSERT INTO csq_1 VALUES (26, 80, 50.55);
INSERT INTO csq_1 VALUES (37, 38, 44.45);
INSERT INTO csq_1 VALUES (52, 9, 43.95);
INSERT INTO csq_1 VALUES (91, 24, 89.62);

INSERT INTO csq_2 VALUES (1, 10, 45.5);
INSERT INTO csq_2 VALUES (2, 20, 67.8);
INSERT INTO csq_2 VALUES (3, 30, 82.1);
INSERT INTO csq_2 VALUES (4, 40, 55.3);
INSERT INTO csq_2 VALUES (5, 50, 71.2);

INSERT INTO csq_3 VALUES (1, 10, 10.0);
INSERT INTO csq_3 VALUES (2, 20, 20.0);
INSERT INTO csq_3 VALUES (3, 30, 30.0);
INSERT INTO csq_3 VALUES (4, 50, 50.0);
INSERT INTO csq_3 VALUES (5, 65, 65.0);
INSERT INTO csq_3 VALUES (6, 70, 70.0);

-- 测试1：基本的复杂子查询（用户提到的场景）
SELECT * FROM csq_1 WHERE feat1 > (SELECT min(csq_2.feat2) FROM csq_2) OR col1 <= (SELECT min(csq_3.col3) FROM csq_3);

-- 测试2：带聚合函数的子查询
SELECT * FROM csq_1 WHERE feat1 > (SELECT max(csq_2.feat2) FROM csq_2);

-- 测试3：EXISTS子查询
SELECT * FROM csq_1 WHERE EXISTS (SELECT * FROM csq_2 WHERE csq_2.id = csq_1.id);

-- 测试4：NOT EXISTS子查询
SELECT * FROM csq_1 WHERE NOT EXISTS (SELECT * FROM csq_2 WHERE csq_2.id > csq_1.id);

-- 测试5：IN子查询
SELECT * FROM csq_1 WHERE id IN (SELECT id FROM csq_2 WHERE feat2 > 50);

-- 测试6：NOT IN子查询
SELECT * FROM csq_1 WHERE id NOT IN (SELECT id FROM csq_2 WHERE feat2 < 60);

-- 测试7：嵌套子查询
SELECT * FROM csq_1 WHERE feat1 > (SELECT max(feat2) FROM csq_2 WHERE id IN (SELECT id FROM csq_3 WHERE col3 > 20));

-- 清理
DROP TABLE csq_1;
DROP TABLE csq_2;
DROP TABLE csq_3;
DROP TABLE csq_4;