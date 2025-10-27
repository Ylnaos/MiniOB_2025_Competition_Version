#!/usr/bin/env python3
"""
测试MiniOB数据插入
用于验证_miniob_code.py的建表和数据插入逻辑
"""
import os
import sys

# 设置环境变量
os.environ["MINIOB_SERVER_SOCKET"] = "/tmp/miniob.sock"  # 根据实际情况修改
os.environ["OB_DOC_PATH"] = "D:/迅雷下载/MiniOB2025/oceanbase-doc/oceanbase-doc/oceanbase-doc"  # 根据实际情况修改

# 导入MiniOB代码
sys.path.insert(0, os.path.dirname(__file__))
from _miniob_code import MiniOBVectorStore

print("=" * 60)
print("  MiniOB Data Insertion Test")
print("=" * 60)

# 模拟Embeddings（简化版）
class MockEmbeddings:
    def embed_query(self, text):
        print(f"[Mock] Embedding query: {text[:50]}...")
        return [0.1] * 1024  # BGE-M3是1024维

    def embed_documents(self, texts):
        print(f"[Mock] Embedding {len(texts)} documents")
        return [[0.1] * 1024 for _ in texts]

# 测试连接和建表
print("\n[Step 1] Testing connection and table creation...")
try:
    embedding = MockEmbeddings()

    vs = MiniOBVectorStore(
        embedding=embedding,
        server_address="127.0.0.1",
        server_port=6789,
        server_socket=os.getenv("MINIOB_SERVER_SOCKET", ""),
        time_limit=10.0,
        charset="utf-8",
        log_func=print
    )
    print("[OK] MiniOB连接成功，表和索引已创建")
except Exception as e:
    print(f"[ERROR] 连接失败: {e}")
    sys.exit(1)

# 测试插入数据
print("\n[Step 2] Testing data insertion...")
try:
    from langchain_core.documents import Document

    test_docs = [
        Document(page_content="OceanBase是分布式数据库", metadata={}),
        Document(page_content="MiniOB是简化版数据库", metadata={}),
        Document(page_content="向量索引使用IVFFlat算法", metadata={})
    ]

    ids = vs.add_documents(test_docs)
    print(f"[OK] 成功插入 {len(ids)} 条文档")
except Exception as e:
    print(f"[ERROR] 插入失败: {e}")
    import traceback
    traceback.print_exc()
    sys.exit(1)

# 测试查询
print("\n[Step 3] Testing similarity search...")
try:
    results = vs.similarity_search("什么是OceanBase", k=2)
    print(f"[OK] 检索到 {len(results)} 条结果:")
    for i, doc in enumerate(results, 1):
        print(f"  {i}. {doc.page_content[:50]}...")
except Exception as e:
    print(f"[ERROR] 查询失败: {e}")
    import traceback
    traceback.print_exc()
    sys.exit(1)

print("\n" + "=" * 60)
print("  All Tests Passed!")
print("=" * 60)
