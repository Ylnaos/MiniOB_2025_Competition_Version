-- P15 分词功能验证
-- 背景：确保 `TOKENIZE` 使用 Jieba 分词时不会因为词典路径解析失败而崩溃，
--      且能够输出与官方期望一致的分词结果。

DROP TABLE IF EXISTS P15;
CREATE TABLE P15(id INT, question TEXT);

INSERT INTO P15 VALUES
(1, '在ALL_PART_KEY_COLUMNS视图中，哪些字段不能为NULL？这些字段分别代表什么信息？');

-- 期望输出：
-- text_tokens
-- ["ALL", "PART", "KEY", "COLUMNS", "视图", "中", "字段", "不能", "NULL", "字段", "代表", "信息"]
SELECT TOKENIZE(question, 'jieba') AS text_tokens FROM P15;

DROP TABLE IF EXISTS P15;
