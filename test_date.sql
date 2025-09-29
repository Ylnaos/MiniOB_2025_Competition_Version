-- Date Type Test
CREATE DATABASE test;
USE test;
CREATE TABLE date_table(id int, u_date date);
INSERT INTO date_table VALUES (1,"2020-01-21");
INSERT INTO date_table VALUES (2,"2020-10-21");
SELECT * FROM date_table;

