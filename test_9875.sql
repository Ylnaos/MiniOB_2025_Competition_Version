-- 创建表9875
create table t9875(id int, age int, name char(20), score float);

-- 插入测试数据
insert into t9875 values(1, 18, 'Alice', 85.5);
insert into t9875 values(2, 22, 'Bob', 90.0);
insert into t9875 values(3, 25, 'Charlie', 78.5);
insert into t9875 values(4, 20, 'David', 92.0);
insert into t9875 values(5, 23, 'Eve', 88.5);

-- 查询所有数据
select * from t9875;

-- 基本查询
select id, name from t9875;
select * from t9875 where age > 20;
select name, score from t9875 where score >= 85.0;

-- 聚合查询
select count(*) from t9875;
select sum(age) from t9875;
select avg(score) from t9875;
select max(score) from t9875;
select min(age) from t9875;

-- 创建视图
create view v9875 as select id, age, name from t9875;
select * from v9875;

-- 创建计算列视图
create view v9875_calc as select id, name, age+10 as future_age, score*1.1 as bonus_score from t9875;
select * from v9875_calc;
select sum(future_age) from v9875_calc;
select avg(bonus_score) from v9875_calc;

-- 清理
drop table t9875;

● -- 创建表9876 - 测试视图列名限制
  create table t9876(id int, age int, name char(20), score float);

  -- 插入测试数据
  insert into t9876 values(1, 18, 'Alice', 85.5);
  insert into t9876 values(2, 22, 'Bob', 90.0);
  insert into t9876 values(3, 25, 'Charlie', 78.5);
  insert into t9876 values(4, 20, 'David', 92.0);
  insert into t9876 values(5, 23, 'Eve', 88.5);

  -- 创建带列名限制的视图（只允许访问id和age）
  create view v9876_limited(id, age) as select id, age from t9876;

  -- 测试1: 查询视图中定义的列 - 应该成功
  select * from v9876_limited;
  select id from v9876_limited;
  select age from v9876_limited;
  select count(*) from v9876_limited;
  select count(id) from v9876_limited;
  select sum(age) from v9876_limited;

  -- 测试2: 查询视图未定义的列 - 应该失败
  select count(name) from v9876_limited;
  select count(score) from v9876_limited;
  select name from v9876_limited;
  select score from v9876_limited;

  -- 创建带列名限制的计算列视图
  create view v9876_calc(user_id, years) as select id, age from t9876;

  -- 测试3: 使用新列名查询 - 应该成功
  select user_id from v9876_calc;
  select years from v9876_calc;
  select count(user_id) from v9876_calc;
  select sum(years) from v9876_calc;

  -- 测试4: 使用原列名查询 - 应该失败
  select id from v9876_calc;
  select age from v9876_calc;

  -- 创建不带列名限制的视图（对比测试）
  create view v9876_unlimited as select id, age, name from t9876;

  -- 测试5: 无限制视图可以访问所有定义的列
  select id from v9876_unlimited;
  select age from v9876_unlimited;
  select name from v9876_unlimited;
  select count(name) from v9876_unlimited;

  -- 清理
  drop view v9876_limited;
  drop view v9876_calc;
  drop view v9876_unlimited;
  drop table t9876;