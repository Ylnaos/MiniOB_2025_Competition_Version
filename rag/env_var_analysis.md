# Langflow 环境变量使用方式分析

## 官方文档 vs 我们的实现

### 官方文档描述的环境变量

文档中的环境变量是用于配置 **Langflow 平台本身**：

- `LANGFLOW_HOST`: Langflow 服务器地址
- `LANGFLOW_PORT`: Langflow 服务器端口
- `LANGFLOW_DATABASE_URL`: Langflow 数据库
- `LANGFLOW_AUTO_LOGIN`: Langflow 自动登录
- ... 等等

这些是 Langflow **平台级配置**，与组件业务逻辑无关。

### 我们使用的环境变量

我们的环境变量是 **业务数据和配置**，用于组件运行时：

- `OB_DOC_PATH`: OceanBase 文档路径（业务数据）
- `EMBEDDING_NAME`: 嵌入模型名称（业务配置）
- `QA_SERVER_GET_QUESTION_URL`: 测评接口地址（业务接口）
- `MINIOB_SERVER_SOCKET`: MiniOB 连接配置（业务配置）

这些是 **组件级配置**，通过 Python 的 `os.getenv()` 读取。

## 两种使用环境变量的方式

### 方式 1: 直接使用 os.getenv()（我们当前的方式）

**代码示例：**
```python
import os

class ChatInput(ChatComponent):
    async def message_response(self) -> Message:
        text = self.input_value
        if not text:
            url = os.getenv("QA_SERVER_GET_QUESTION_URL")  # 直接读取
            response = requests.get(url)
            text = response.json().get("question")
```

**优点：**
- ✅ 简单直接
- ✅ 标准 Python 方式
- ✅ 不依赖 Langflow 全局变量系统
- ✅ 适合测评环境（操作系统级环境变量）

**缺点：**
- ❌ UI 中看不到变量值
- ❌ 需要在操作系统级别设置

### 方式 2: 使用 Langflow 全局变量系统

**Langflow 提供的机制：**

1. 设置 `LANGFLOW_VARIABLES_TO_GET_FROM_ENVIRONMENT`：
```bash
LANGFLOW_VARIABLES_TO_GET_FROM_ENVIRONMENT=OB_DOC_PATH,EMBEDDING_NAME,QA_SERVER_GET_QUESTION_URL
```

2. Langflow 会自动将这些环境变量导入为全局变量

3. 在组件中引用全局变量（而不是直接用 os.getenv）

**但这需要修改组件代码，且测评环境可能不支持这种方式。**

## 相关的 Langflow 配置

根据官方文档，以下配置影响环境变量的使用：

### LANGFLOW_FALLBACK_TO_ENV_VAR（默认 true）

```
如果启用，Langflow UI 中设置的全局变量在 Langflow 无法检索变量值时，
会回退到同名的环境变量。
```

**含义**：即使我们在 UI 中定义了全局变量，如果值为空，Langflow 会自动从
操作系统环境变量中读取同名变量。

**对我们的影响**：
- 这是**好消息**！说明 Langflow 本身支持从操作系统环境变量读取
- 我们使用 `os.getenv()` 的方式是被官方支持的

### LANGFLOW_STORE_ENVIRONMENT_VARIABLES（默认 true）

```
将环境变量作为全局变量存储在数据库中。
```

**含义**：Langflow 会自动将某些环境变量存储为全局变量。

### LANGFLOW_VARIABLES_TO_GET_FROM_ENVIRONMENT

```
从环境中获取并存储为全局变量的环境变量的逗号分隔列表。
```

**如果设置了这个**：
```bash
LANGFLOW_VARIABLES_TO_GET_FROM_ENVIRONMENT=OB_DOC_PATH,EMBEDDING_NAME
```

Langflow 会自动将 `OB_DOC_PATH` 和 `EMBEDDING_NAME` 从操作系统环境变量
导入为全局变量。

## 我们的方案评估

### ✅ 当前方案（使用 os.getenv()）

**优点：**
1. ✅ 符合 Python 标准实践
2. ✅ 直接读取操作系统环境变量
3. ✅ 不依赖 Langflow 特定功能
4. ✅ 测评环境设置的环境变量可以直接使用
5. ✅ 有 `LANGFLOW_FALLBACK_TO_ENV_VAR` 的官方支持

**验证当前方案的正确性：**
- Langflow 官方文档中的 `LANGFLOW_FALLBACK_TO_ENV_VAR=true` 说明
  Langflow **本身就支持**从操作系统环境变量读取
- 我们的 `os.getenv()` 方式是标准的 Python 环境变量读取方式
- 测评环境会在操作系统级别设置这些变量（通过 Docker 环境变量或 systemd）

### ❓ 可选方案（使用 Langflow 全局变量）

如果测评环境设置了：
```bash
LANGFLOW_VARIABLES_TO_GET_FROM_ENVIRONMENT=OB_DOC_PATH,EMBEDDING_NAME,QA_SERVER_GET_QUESTION_URL,QA_SERVER_POST_ANSWER_URL,MINIOB_SERVER_SOCKET,LLM_NAME,LLM_API_KEY,LLM_BASE_URL,EMBEDDING_BASE_URL
```

则可以在组件中通过 Langflow 的全局变量系统访问这些值。

**但这不是必需的！** 我们当前的 `os.getenv()` 方式已经足够。

## 结论

### ✅ 我们的理解是正确的

1. **官方文档中的环境变量** = Langflow 平台配置
2. **我们使用的环境变量** = 业务组件配置
3. **两者是不同层次的概念**

### ✅ 我们的实现方式是正确的

1. 使用 `os.getenv()` 直接读取操作系统环境变量
2. UI 输入框为空是正常的（默认值）
3. 测评环境会在操作系统级别设置环境变量
4. Langflow 的 `LANGFLOW_FALLBACK_TO_ENV_VAR=true` 支持这种方式

### ✅ 验证方法不变

运行时查看日志输出：
```
[ChatInput] QA_SERVER_GET_QUESTION_URL: http://...
[TestOutput] QA_SERVER_POST_ANSWER_URL: http://...
```

如果看到实际的 URL，说明环境变量被正确读取。
如果看到错误提示，说明测评环境未设置环境变量。

## 测评环境应该如何设置

测评环境应该在启动 Langflow 之前设置这些环境变量：

```bash
# 方式 1: 在 shell 中 export
export OB_DOC_PATH=/path/to/docs
export EMBEDDING_NAME=bge-m3
export QA_SERVER_GET_QUESTION_URL=http://test-server/api/question
# ... 其他变量

# 启动 Langflow
python -m langflow run

# 方式 2: 使用 Docker 环境变量
docker run -e OB_DOC_PATH=/path/to/docs \
           -e EMBEDDING_NAME=bge-m3 \
           -e QA_SERVER_GET_QUESTION_URL=http://... \
           langflow-image

# 方式 3: 使用 systemd EnvironmentFile
# /etc/systemd/system/langflow.service
[Service]
EnvironmentFile=/path/to/langflow.env
ExecStart=python -m langflow run
```

## 最终确认

✅ **UI 输入框为空是正确的**
✅ **使用 os.getenv() 是正确的**
✅ **不需要使用 Langflow 全局变量系统**
✅ **测评环境会设置操作系统级环境变量**
✅ **我们的配置完全正确**
