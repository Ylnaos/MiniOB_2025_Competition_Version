-- 使用前请先启动 MiniOB 并连接到默认数据库
-- 建议：将向量维度与嵌入模型保持一致。bge-m3 的输出维度通常为 1024。

-- 1) 文档表：存储分块后的文本与向量
DROP TABLE IF EXISTS ob_docs;
CREATE TABLE ob_docs (
  id        INT,                   -- 自增ID（可用序号脚本或客户端生成）
  path      STRING(512) NULL,      -- 文档源路径/标题
  title     STRING(256) NULL,      -- 可选：分块标题
  content   TEXT NULL,             -- 分块正文
  embedding VECTOR(1024) NULL      -- 嵌入向量（根据模型维度调整）
);

-- 2) 基于 IVF-Flat 的向量索引（余弦距离）。lists/probes 可按数据规模微调
DROP INDEX IF EXISTS idx_ob_docs_vec ON ob_docs;
CREATE VECTOR INDEX idx_ob_docs_vec ON ob_docs { embedding }
  WITH { TYPE = IVFFLAT, DISTANCE = COSINE_DISTANCE, LISTS = 100, PROBES = 10 };

-- 可选：关键字检索辅助（若需要）
-- DROP INDEX IF EXISTS ft_ob_docs_content ON ob_docs;
-- CREATE FULLTEXT INDEX ft_ob_docs_content ON ob_docs { content };

