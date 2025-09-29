-- Drop Table Test
CREATE DATABASE droptest;
USE droptest;
CREATE TABLE test_table(id int, name char);
INSERT INTO test_table VALUES (1,"OB");
SELECT * FROM test_table;
DROP TABLE test_table;
SELECT * FROM test_table;

