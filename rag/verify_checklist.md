# Langflow 环境变量配置验证清单

## 重要概念

**环境变量不会显示在 UI 输入框中！**

- UI 输入框显示的是 **默认值（fallback value）**
- 环境变量在 **代码运行时** 通过 `os.getenv()` 动态读取
- 如果环境变量未设置，则使用默认值

## 工作流程

```
1. 导入 model.json 到 Langflow
   ↓
2. UI 显示各组件的默认值（通常为空）← 这是正常的！
   ↓
3. 运行时执行: os.getenv("ENV_VAR", default)
   ↓
4. 优先使用环境变量，否则使用默认值
```

## 验证清单

### ✓ 第1步：确认代码中有 os.getenv() 调用

运行以下命令检查：

```bash
cd D:\迅雷下载\MiniOB2025\MiniOB_2025_Competition_Version\rag
python -c "
import json
data = json.load(open('model.json', encoding='utf-8'))
print('Environment Variables Used:')
for node in data['data']['nodes']:
    code = node['data']['node'].get('template', {}).get('code', {}).get('value', '')
    if 'os.getenv' in code:
        component = node['data']['id'].split('-')[0]
        env_vars = []
        for line in code.split('\n'):
            if 'os.getenv(\"' in line:
                start = line.find('os.getenv(\"') + 11
                end = line.find('\"', start)
                if end > start:
                    var = line[start:end]
                    if var not in env_vars:
                        env_vars.append(var)
        if env_vars:
            print(f'  {component}: {env_vars}')
"
```

**期望输出：**
```
Environment Variables Used:
  Directory: ['OB_DOC_PATH']
  OllamaEmbeddings: ['EMBEDDING_NAME', 'EMBEDDING_BASE_URL']
  ChatQwenModel: ['LLM_NAME', 'LLM_API_KEY', 'LLM_BASE_URL']
  MiniOB: ['MINIOB_SERVER_SOCKET']
  ChatInput: ['QA_SERVER_GET_QUESTION_URL']
  TestOutput: ['QA_SERVER_POST_ANSWER_URL']
```

### ✓ 第2步：确认 UI 显示是正常的

**在 Langflow UI 中，你应该看到：**

| 组件 | 字段 | UI 显示值 | 说明 |
|------|------|----------|------|
| Directory | Path | **空** | ✓ 正常！运行时读取 OB_DOC_PATH |
| OllamaEmbeddings | Model Name | **空** | ✓ 正常！运行时读取 EMBEDDING_NAME |
| OllamaEmbeddings | Base URL | `http://localhost:11434` | ✓ 正常！默认值 |
| ChatQwenModel | Model Name | `qwen-plus` | ✓ 正常！默认值 |
| ChatQwenModel | API Key | **空** | ✓ 正常！运行时读取 LLM_API_KEY |
| ChatInput | Input Text | **空** | ✓ 正常！运行时从测评服务器获取 |
| MiniOB | Server Socket | **空** | ✓ 正常！运行时读取 MINIOB_SERVER_SOCKET |

**如果看到输入框为空 → 这是正确的！** ✓

### ✓ 第3步：运行时验证

运行工作流后，查看日志输出：

**成功的日志示例：**
```
[ChatInput] QA_SERVER_GET_QUESTION_URL: http://test-server/question
[ChatInput] Extracted question: '什么是UNDO和REDO？'
[Directory] Using path: /path/to/OceanBase/docs
[OllamaEmbeddings] Using model: bge-m3
[TestOutput] QA_SERVER_POST_ANSWER_URL: http://test-server/answer
[TestOutput] Response status code: 200
```

**失败的日志示例（环境变量未设置）：**
```
[ChatInput] ERROR: QA_SERVER_GET_QUESTION_URL not set!
```

### ✓ 第4步：测评环境验证

在测评环境中执行：

```bash
# 检查环境变量是否设置
echo "OB_DOC_PATH=$OB_DOC_PATH"
echo "EMBEDDING_NAME=$EMBEDDING_NAME"
echo "EMBEDDING_BASE_URL=$EMBEDDING_BASE_URL"
echo "LLM_NAME=$LLM_NAME"
echo "LLM_API_KEY=$LLM_API_KEY"
echo "LLM_BASE_URL=$LLM_BASE_URL"
echo "QA_SERVER_GET_QUESTION_URL=$QA_SERVER_GET_QUESTION_URL"
echo "QA_SERVER_POST_ANSWER_URL=$QA_SERVER_POST_ANSWER_URL"
echo "MINIOB_SERVER_SOCKET=$MINIOB_SERVER_SOCKET"
```

**如果任何变量为空，说明测评环境配置有问题。**

## 常见误解

### ❌ 错误理解
"UI 输入框应该显示 `$OB_DOC_PATH` 或 `${OB_DOC_PATH}`"

### ✓ 正确理解
"UI 输入框显示默认值（通常为空），环境变量在代码运行时读取"

## 代码示例

### Directory 组件代码片段：
```python
def build_dataframe(self) -> DataFrame:
    # 运行时读取环境变量，UI 不显示
    self.path = os.getenv("OB_DOC_PATH", self.path)
    path = self.path
    # ... 后续逻辑
```

### ChatInput 组件代码片段：
```python
async def message_response(self) -> Message:
    text = self.input_value
    if not text:
        # 运行时读取环境变量
        url = os.getenv("QA_SERVER_GET_QUESTION_URL")
        response = requests.get(url)
        # ... 后续逻辑
```

## 总结

✓ **UI 输入框为空是正常的** - 不用担心！
✓ **环境变量在代码中通过 os.getenv() 读取** - 已验证！
✓ **测评环境会自动设置这些环境变量** - 测评方负责！
✓ **运行时日志会显示实际使用的值** - 可以调试！

---

**如果还有疑问，请查看运行时日志输出！**
