-- 测试聚合视图的COUNT(*)
create table create_view_t125(id int, age int, name char(10));
insert into create_view_t125 values(1, 18, 'a');
insert into create_view_t125 values(2, 20, 'b');

-- 创建包含聚合函数的视图
create view create_view_v71 as select count(t1.id) as num, sum(t1.age)+sum(t2.age) as data from create_view_t125 t1, create_view_t125 t2 where t1.id=t2.id;

-- 查看视图内容（应该返回1行）
select * from create_view_v71;

-- 对视图执行COUNT(*)（应该返回1）
select count(*) from create_view_v71;
