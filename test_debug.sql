-- Test 1: 加法表达式（已验证成功）
create table t_add(id int, age int);
insert into t_add values(1, 10);
create view v_add as select id, id+age as total from t_add;
select sum(total) from v_add;

-- Test 2: 乘法表达式（报告失败）
create table t_mul(id int, value int);
insert into t_mul values(1, 100);
create view v_mul as select id, value*2 as double_value from t_mul;
select sum(double_value) from v_mul;
