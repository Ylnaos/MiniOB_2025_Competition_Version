-- 测试TOKENIZE函数修复结果
-- 创建测试表
CREATE TABLE test_texts (id INT,content TEXTS);

-- 插入测试数据
INSERT INTO test_texts (id, content) VALUES (1, '文件状态FILE_STATUS有哪些可能的值，它们分别代表什么意思？'),(2, '你好世界'),(3, 'Hello world'),(4, '数据库管理系统'),(5, '这是一个测试用例');

-- 测试TOKENIZE函数
-- 1. 基本分词测试
SELECT id, content, TOKENIZE(content, 'jieba') as tokens FROM test_texts WHERE id = 1;

-- 2. 英文分词测试
SELECT id, content, TOKENIZE(content, 'jieba') as tokens FROM test_texts WHERE id = 3;

-- 3. 中文分词测试
SELECT id, content, TOKENIZE(content, 'jieba') as tokens FROM test_texts WHERE id = 2;

-- 4. 混合中英文测试
SELECT id, content, TOKENIZE(content, 'jieba') as tokens FROM test_texts WHERE id = 4;

-- 5. 空值测试
SELECT TOKENIZE(NULL, 'jieba') as tokens;

-- 6. 无参数测试（应该默认使用jieba）
SELECT TOKENIZE('测试默认分词器') as tokens;

-- 清理测试表
DROP TABLE test_texts;