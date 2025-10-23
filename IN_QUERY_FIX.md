# IN 查询修复方案

## 问题分析

IN 查询返回 FAILURE 的可能原因：

### 1. 解析路径问题
- `WHERE id IN (...)` 可能同时匹配 `condition` 和 `condition_expr` 规则
- yacc 默认选择先定义的规则（condition 路径）
- condition 路径通过 FilterStmt 处理

### 2. 可能的失败点

#### 检查点1: CompOp 范围验证
文件：`src/observer/sql/stmt/filter_stmt.cpp:215-218`
```cpp
if (comp < EQUAL_TO || comp >= NO_OP) {
  LOG_WARN("invalid compare operator : %d", comp);
  return RC::INVALID_ARGUMENT;
}
```
- IN_OP=8, NOT_IN_OP=9, NO_OP=12
- 应该通过检查

#### 检查点2: SubqueryExpr 状态
- 值列表形式的 SubqueryExpr 通过构造函数创建
- executed_=true, results_ 包含值列表
- copy() 应该正确复制状态

#### 检查点3: InExpr 执行
文件：`src/observer/sql/expr/expression.cpp:3582-3585`
```cpp
if (set_expr_->type() != ExprType::SUBQUERY) {
  LOG_WARN("IN operator's right expr should be a subquery");
  return RC::INVALID_ARGUMENT;
}
```
- 如果 set_expr_ 不是 SUBQUERY 类型，会返回 INVALID_ARGUMENT

## 修复方案

### 方案1：确保 SubqueryExpr 类型不变
确保在整个处理流程中，IN 的右侧表达式始终保持 SUBQUERY 类型。

### 方案2：添加调试日志
在关键路径添加日志，追踪 IN 查询的处理流程。

### 方案3：检查 LBRACE/RBRACE token
确保 `IN (...)` 中的括号被正确解析为 LBRACE 和 RBRACE，而不是 LBRACKET。

## 测试用例

```sql
-- 基础测试
SELECT * FROM test WHERE id IN (1, 2, 3);

-- 重复值测试
SELECT * FROM test WHERE id IN (1,1,2,2,3);

-- 单值测试
SELECT * FROM test WHERE id IN (1);

-- NOT IN 测试
SELECT * FROM test WHERE id NOT IN (1, 2);
```

## 下一步

1. 用户提供详细错误日志
2. 根据日志定位具体失败点
3. 应用针对性修复
