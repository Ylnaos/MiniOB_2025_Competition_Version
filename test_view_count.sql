create view create_view_v7 as select count(t1.id) as num, sum(t1.age)+sum(t2.age) as data from create_view_t1 t1, create_view_t1 t2 where t1.id=t2.id;select count(*) from create_view_v7;期望输出 1
-- below are some requests executed before(partial) --
-- init data
create table create_view_t1(id int, age int, name char(10));
-- init table2
create table create_view_t2(id int, age int, name char(10));