# RAG工作流问题诊断报告

## 问题现象

Observer日志显示：
```
Listen on port 6789
Observer start success
Accepted connection from 127.0.0.1:12199
worker thread start. communicator = 0x50b000010000
Accepted connection from 127.0.0.1:56748
worker thread start. communicator = 0x50b000010160
Accepted connection from 127.0.0.1:8616
worker thread start. communicator = 0x50b000010210
```

**问题**：只有连接建立，没有任何SQL语句执行（CREATE TABLE, INSERT等）

---

## 根本原因

### 1. 工作流触发机制

Langflow RAG工作流是**事件驱动**的，需要测评服务器触发：

```
测评服务器发送问题 → ChatInput获取 → 触发RAG流程 → TestOutput提交答案
```

**关键组件：**
- **ChatInput**: 从 `QA_SERVER_GET_QUESTION_URL` 获取问题
- **TestOutput**: 向 `QA_SERVER_POST_ANSWER_URL` 提交答案

**当前状态：**
- Langflow已启动并连接到MiniOB（3个worker threads）
- 但没有从测评服务器收到问题
- 因此工作流未执行数据插入阶段

### 2. 数据插入时机

MiniOB的数据插入发生在：

```python
# 位置：_miniob_code.py::MiniOBVectorStoreComponent.build_vector_store()

def build_vector_store(self):
    vector_store = MiniOBVectorStore(...)  # 触发__init__，执行建表和索引创建

    if not self._already_build():         # 检查表是否已有数据
        self._add_documents_to_vector_store(vector_store)  # 插入文档

    return vector_store
```

**触发条件：**
- `build_vector_store()`被调用
- `ingest_data`输入有数据（从Directory → SplitText传来）

### 3. 必需环境变量

```bash
# 文档路径
OB_DOC_PATH=/path/to/oceanbase-doc

# MiniOB连接
MINIOB_SERVER_SOCKET=/tmp/miniob.sock

# 测评服务器（可选，用于自动测评）
QA_SERVER_GET_QUESTION_URL=http://评测机/get_question
QA_SERVER_POST_ANSWER_URL=http://评测机/post_answer

# Embedding模型
EMBEDDING_NAME=bge-m3
EMBEDDING_BASE_URL=http://embedding-service

# LLM
LLM_NAME=qwen
LLM_API_KEY=your-api-key
LLM_BASE_URL=http://llm-service
```

---

## 解决方案

### 方案1：手动测试数据插入（推荐用于调试）

运行测试脚本验证SQL逻辑：

```bash
# 修改test_insert.py中的路径
cd D:\迅雷下载\MiniOB2025\MiniOB_2025_Competition_Version\rag

# 设置环境变量
export MINIOB_SERVER_SOCKET=/tmp/miniob.sock
export OB_DOC_PATH=/path/to/oceanbase-doc

# 运行测试
python test_insert.py
```

**预期结果：**
- Observer日志出现：
  ```
  CREATE TABLE rag_docs (content TEXT, embedding VECTOR(1024))
  CREATE VECTOR INDEX idx_rag_docs_embedding ON rag_docs (embedding) WITH (...)
  INSERT INTO rag_docs (content, embedding) VALUES (...)
  ```

### 方案2：完整测评流程

需要启动测评服务器：

```bash
# 1. 设置所有环境变量
export OB_DOC_PATH=/path/to/oceanbase-doc
export MINIOB_SERVER_SOCKET=/tmp/miniob.sock
export QA_SERVER_GET_QUESTION_URL=http://测评机:port/get_question
export QA_SERVER_POST_ANSWER_URL=http://测评机:port/post_answer
export EMBEDDING_NAME=bge-m3
export EMBEDDING_BASE_URL=http://localhost:port

# 2. 启动MiniOB
/root/miniob/build/bin/observer -s /tmp/miniob.sock -P mysql

# 3. 启动Langflow
langflow run --host 0.0.0.0 --port 7860

# 4. 加载工作流
# 在Langflow UI中导入 rag/model.json

# 5. 运行工作流
# 点击运行按钮，或等待测评服务器发送问题
```

### 方案3：检查配置

验证当前配置：

```bash
cd D:\迅雷下载\MiniOB2025\MiniOB_2025_Competition_Version\rag

python -c "
import json
with open('model.json') as f:
    model = json.load(f)

# 检查edges
edges = model['data']['edges']
print(f'Edges count: {len(edges)}')

# 检查MiniOB连接
for edge in edges:
    if 'MiniOB' in edge['target']:
        print(f\"  Input: {edge['source'].split('-')[0]} => {edge['data']['targetHandle']['fieldName']}\")
"
```

---

## 诊断清单

- [ ] MiniOB Observer正在运行（端口6789）
- [ ] Langflow已启动并连接到MiniOB（看到worker threads）
- [ ] 环境变量OB_DOC_PATH已设置且路径正确
- [ ] 环境变量MINIOB_SERVER_SOCKET已设置
- [ ] model.json中有9-10条edges连线
- [ ] Directory → SplitText → MiniOB连线正确
- [ ] 测试脚本test_insert.py能成功执行
- [ ] Observer日志出现CREATE TABLE和INSERT语句

---

## 常见问题

**Q1: Observer只有连接，没有SQL语句？**
A: 工作流未被触发。运行test_insert.py手动触发数据插入。

**Q2: CREATE TABLE失败？**
A: 检查SQL语法是否正确（已在_miniob_code.py中修复）。

**Q3: INSERT失败？**
A: 检查：
   - VECTOR维度是否为1024
   - 文本是否过长（已限制1000字节）
   - 向量数据格式是否正确

**Q4: 向量检索返回空结果？**
A: 检查：
   - 表中是否有数据（`SELECT COUNT(*) FROM rag_docs`）
   - 索引是否创建成功
   - COSINE_DISTANCE函数语法是否正确（已修复）

---

## 下一步

1. **运行test_insert.py** - 验证数据插入逻辑
2. **检查Observer日志** - 确认SQL语句执行
3. **设置完整环境变量** - 准备测评环境
4. **运行完整工作流** - 提交测评
