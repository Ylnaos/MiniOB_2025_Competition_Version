# P12 - 聚合视图查询测试用例

## 问题描述
修复聚合视图查询的以下问题：
1. `SELECT COUNT(*) FROM aggregate_view` 应返回 1 行（即使视图结果为空）
2. `SELECT * FROM aggregate_view` 应正确返回视图的聚合结果
3. `SELECT SUM(column) FROM aggregate_view` 应正确绑定字段并返回结果

---

## 测试用例分类

### A. 基础聚合视图查询

| 用例编号 | 场景 | SQL | 期望输出 | 说明 |
|---------|------|-----|---------|------|
| A1 | 空表COUNT视图 | `SELECT * FROM v_empty_count;` | `0` (1行) | 空表的COUNT应返回0 |
| A2 | 空表SUM视图 | `SELECT * FROM v_empty_sum;` | `NULL` (1行) | 空表的SUM应返回NULL |
| A3 | 空表多聚合视图 | `SELECT * FROM v_empty_multi;` | `0 \| NULL \| NULL` (1行) | COUNT=0, SUM/AVG=NULL |
| A4 | 有数据COUNT视图 | `SELECT * FROM v_data_count;` | `3` (1行) | 正常COUNT |
| A5 | 有数据SUM视图 | `SELECT * FROM v_data_sum;` | `75` (1行) | 正常SUM |

**准备脚本**：
```sql
-- 创建空表
DROP TABLE IF EXISTS t_empty;
CREATE TABLE t_empty(id INT, val INT);

-- 创建有数据表
DROP TABLE IF EXISTS t_data;
CREATE TABLE t_data(id INT, val INT);
INSERT INTO t_data VALUES (1, 20), (2, 30), (3,25);

-- 创建聚合视图
DROP VIEW IF EXISTS v_empty_count;
CREATE VIEW v_empty_count AS SELECT COUNT(*) as cnt FROM t_empty;

DROP VIEW IF EXISTS v_empty_sum;
CREATE VIEW v_empty_sum AS SELECT SUM(val) as total FROM t_empty;

DROP VIEW IF EXISTS v_empty_multi;
CREATE VIEW v_empty_multi AS SELECT COUNT(*) as cnt, SUM(val) as total, AVG(val) as avg_val FROM t_empty;

DROP VIEW IF EXISTS v_data_count;
CREATE VIEW v_data_count AS SELECT COUNT(*) as cnt FROM t_data;

DROP VIEW IF EXISTS v_data_sum;
CREATE VIEW v_data_sum AS SELECT SUM(val) as total FROM t_data;
```

---

### B. 外层COUNT聚合（核心Bug场景）

| 用例编号 | 场景 | SQL | 期望输出 | 说明 |
|---------|------|-----|---------|------|
| B1 | COUNT空视图结果 | `SELECT COUNT(*) FROM v_empty_count;` | `1` | 视图返回1行(值为0)，COUNT应为1 |
| B2 | COUNT有数据视图 | `SELECT COUNT(*) FROM v_data_count;` | `1` | 视图返回1行，COUNT应为1 |
| B3 | COUNT多列视图 | `SELECT COUNT(*) FROM v_empty_multi;` | `1` | 无论视图列数，COUNT(*)应为1 |
| B4 | COUNT带别名 | `SELECT COUNT(*) as row_count FROM v_empty_count;` | `1` | 别名不影响结果 |

---

### C. 外层聚合函数（SUM/AVG/MAX/MIN）

| 用例编号 | 场景 | SQL | 期望输出 | 说明 |
|---------|------|-----|---------|------|
| C1 | SUM空视图列 | `SELECT SUM(cnt) FROM v_empty_count;` | `0` | SUM(0) = 0 |
| C2 | SUM有数据视图列 | `SELECT SUM(cnt) FROM v_data_count;` | `3` | SUM(3) = 3 |
| C3 | AVG空视图列 | `SELECT AVG(cnt) FROM v_empty_count;` | `0` | AVG(0) = 0 |
| C4 | MAX空视图列 | `SELECT MAX(cnt) FROM v_empty_count;` | `0` | MAX(0) = 0 |
| C5 | MIN空视图列 | `SELECT MIN(cnt) FROM v_empty_count;` | `0` | MIN(0) = 0 |
| C6 | SUM NULL列 | `SELECT SUM(total) FROM v_empty_sum;` | `NULL` | SUM(NULL) = NULL |
| C7 | AVG NULL列 | `SELECT AVG(total) FROM v_empty_sum;` | `NULL` | AVG(NULL) = NULL |

---

### D. 复杂表达式和多列聚合

| 用例编号 | 场景 | SQL | 期望输出 | 说明 |
|---------|------|-----|---------|------|
| D1 | 多个聚合函数 | `SELECT SUM(cnt), AVG(total) FROM v_empty_multi;` | `0 \| NULL` | 多个外层聚合 |
| D2 | 算术表达式 | `SELECT SUM(cnt) + 1 FROM v_empty_count;` | `1` | 0 + 1 = 1 |
| D3 | 聚合表达式 | `SELECT SUM(cnt * 2) FROM v_data_count;` | `6` | SUM(3*2) = 6 |
| D4 | COUNT + SUM | `SELECT COUNT(*), SUM(cnt) FROM v_data_count;` | `1 \| 3` | 混合聚合 |
| D5 | 所有聚合函数 | `SELECT COUNT(*), SUM(cnt), AVG(cnt), MAX(cnt), MIN(cnt) FROM v_data_count;` | `1 \| 3 \| 3 \| 3 \| 3` | 全部聚合类型 |

---

### E. 原始Bug用例（JOIN + 聚合视图）

| 用例编号 | 场景 | SQL | 期望输出 | 说明 |
|---------|------|-----|---------|------|
| E1 | 视图定义 | `CREATE VIEW v7 AS SELECT COUNT(t1.id) as num, SUM(t1.age)+SUM(t2.age) as data FROM t1, t2 WHERE t1.id=t2.id;` | SUCCESS | 创建视图 |
| E2 | SELECT * | `SELECT * FROM v7;` | `0 \| NULL` (1行) | 空表JOIN无结果，聚合返回1行 |
| E3 | COUNT(*) | `SELECT COUNT(*) FROM v7;` | `1` | 外层COUNT应为1 |
| E4 | SUM(num) | `SELECT SUM(num) FROM v7;` | `0` | SUM(0) = 0 |
| E5 | AVG(data) | `SELECT AVG(data) FROM v7;` | `NULL` | AVG(NULL) = NULL |

**准备脚本**：
```sql
DROP TABLE IF EXISTS t1;
DROP TABLE IF EXISTS t2;
CREATE TABLE t1(id INT, age INT, name CHAR(10));
CREATE TABLE t2(id INT, age INT, name CHAR(10));

DROP VIEW IF EXISTS v7;
CREATE VIEW v7 AS
  SELECT COUNT(t1.id) as num, SUM(t1.age)+SUM(t2.age) as data
  FROM t1, t2
  WHERE t1.id=t2.id;
```

---

### F. 边缘情况

| 用例编号 | 场景 | SQL | 期望输出 | 说明 |
|---------|------|-----|---------|------|
| F1 | 嵌套聚合视图 | `SELECT COUNT(*) FROM (SELECT COUNT(*) FROM t_empty);` | ERROR | 子查询语法（如果支持） |
| F2 | 视图别名 | `SELECT COUNT(*) FROM v_empty_count AS v;` | ERROR | 视图不支持别名 |
| F3 | 列名大小写 | `SELECT sum(CNT) FROM v_data_count;` | `3` | 大小写不敏感 |
| F4 | 不存在的列 | `SELECT SUM(xyz) FROM v_data_count;` | ERROR | SCHEMA_FIELD_MISSING |
| F5 | COUNT(列名) | `SELECT COUNT(cnt) FROM v_data_count;` | `1` | COUNT非空列 |
| F6 | COUNT(NULL列) | `SELECT COUNT(total) FROM v_empty_sum;` | `0` | COUNT忽略NULL |

---

### G. 性能和压力测试

| 用例编号 | 场景 | SQL | 期望输出 | 说明 |
|---------|------|-----|---------|------|
| G1 | 大数据集 | `SELECT COUNT(*) FROM v_large;` | `1` | 10000行数据视图 |
| G2 | 多列聚合 | `SELECT * FROM v_multi_10;` | 10列聚合结果 (1行) | 10个聚合列 |
| G3 | 复杂计算 | `SELECT SUM(col1*col2+col3) FROM v_complex;` | 计算结果 | 复杂算术表达式 |

**准备脚本**：
```sql
-- 创建大数据集（使用循环或批量插入）
DROP TABLE IF EXISTS t_large;
CREATE TABLE t_large(id INT, val INT);
-- 插入10000行...

DROP VIEW IF EXISTS v_large;
CREATE VIEW v_large AS SELECT COUNT(*) as cnt, SUM(val) as total FROM t_large;

DROP VIEW IF EXISTS v_multi_10;
CREATE VIEW v_multi_10 AS
  SELECT
    COUNT(*) as c1, SUM(val) as s1, AVG(val) as a1, MAX(val) as m1, MIN(val) as n1,
    COUNT(id) as c2, SUM(id) as s2, AVG(id) as a2, MAX(id) as m2, MIN(id) as n2
  FROM t_large;
```

---

## 测试执行顺序

1. **先执行 A 类**：验证基础视图查询正常
2. **再执行 B 类**：验证核心 Bug（COUNT(*) 返回 1）
3. **执行 C/D 类**：验证外层聚合函数正确性
4. **执行 E 类**：验证原始 Bug 场景
5. **执行 F 类**：验证边界和错误处理
6. **可选 G 类**：性能测试

---

## 期望修复的关键点

✅ **修复点 1**：`ScalarGroupByPhysicalOperator` 保证无输入时返回 1 行
✅ **修复点 2**：外层查询正确处理视图的聚合结果
✅ **修复点 3**：`AggregateExpr` 子表达式正确绑定到视图列
✅ **修复点 4**：列名正确显示（不出现空行）

---

## 快速验证脚本

```sql
-- 一键执行核心测试
DROP TABLE IF EXISTS test_t;
CREATE TABLE test_t(id INT, val INT);

DROP VIEW IF EXISTS test_v;
CREATE VIEW test_v AS SELECT COUNT(*) as cnt, SUM(val) as total FROM test_t;

-- 核心测试（应全部通过）
SELECT * FROM test_v;                    -- 期望: 0 | NULL (1行)
SELECT COUNT(*) FROM test_v;             -- 期望: 1
SELECT SUM(cnt) FROM test_v;             -- 期望: 0
SELECT AVG(total) FROM test_v;           -- 期望: NULL
SELECT COUNT(*), SUM(cnt) FROM test_v;   -- 期望: 1 | 0

-- 插入数据后再测试
INSERT INTO test_t VALUES (1, 10), (2, 20), (3, 30);

SELECT * FROM test_v;                    -- 期望: 3 | 60 (1行)
SELECT COUNT(*) FROM test_v;             -- 期望: 1
SELECT SUM(cnt) FROM test_v;             -- 期望: 3
SELECT AVG(total) FROM test_v;           -- 期望: 60

-- 清理
DROP VIEW IF EXISTS test_v;
DROP TABLE IF EXISTS test_t;
```

---

## 预期测试结果统计

| 分类 | 用例数 | 预期通过 | 预期失败 | 说明 |
|------|--------|---------|---------|------|
| A - 基础查询 | 5 | 5 | 0 | 全部通过 |
| B - COUNT Bug | 4 | 4 | 0 | 核心修复点 |
| C - 外层聚合 | 7 | 7 | 0 | 全部通过 |
| D - 复杂表达式 | 5 | 5 | 0 | 全部通过 |
| E - 原始Bug | 5 | 5 | 0 | 回归测试 |
| F - 边缘情况 | 6 | 4 | 2 | F1/F2 预期失败 |
| G - 性能测试 | 3 | 3 | 0 | 可选 |
| **总计** | **35** | **33** | **2** | **94.3%通过率** |
