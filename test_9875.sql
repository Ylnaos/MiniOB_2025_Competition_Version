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
