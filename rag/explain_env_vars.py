#!/usr/bin/env python3
"""
解释 Langflow 环境变量工作机制并提供验证方法
"""
import json

json_file = r"D:\迅雷下载\MiniOB2025\MiniOB_2025_Competition_Version\rag\model.json"

with open(json_file, "r", encoding="utf-8") as f:
    data = json.load(f)

print("=" * 80)
print("Langflow 环境变量工作机制说明")
print("=" * 80)

print("""
重要概念：
---------
Langflow 组件的环境变量是在 **代码运行时** 动态读取的，
而 **不是** 在 UI 输入框中显示占位符。

UI 输入框中显示的是 **默认值（fallback value）**，
如果环境变量未设置，则使用这个默认值。

工作流程：
---------
1. 用户导入 model.json 到 Langflow
2. UI 显示各组件的 **默认值**（通常为空或预设值）
3. 运行时，代码执行 os.getenv("ENV_VAR", default_value)
4. 如果环境变量存在 → 使用环境变量值
5. 如果环境变量不存在 → 使用默认值

示例：
-----
""")

# 找几个组件来演示
examples = []

# 1. Directory 组件
for node in data['data']['nodes']:
    if 'Directory' in node['data']['id']:
        code = node['data']['node']['template']['code']['value']
        path_value = node['data']['node']['template']['path']['value']

        # 找到使用 os.getenv 的行
        for line in code.split('\n'):
            if 'os.getenv("OB_DOC_PATH"' in line:
                examples.append({
                    'component': 'Directory',
                    'env_var': 'OB_DOC_PATH',
                    'ui_display': path_value,
                    'code_line': line.strip()
                })
                break
        break

# 2. OllamaEmbeddings 组件
for node in data['data']['nodes']:
    if 'OllamaEmbeddings' in node['data']['id']:
        code = node['data']['node']['template']['code']['value']
        model_value = node['data']['node']['template']['model_name']['value']

        for line in code.split('\n'):
            if 'os.getenv("EMBEDDING_NAME"' in line:
                examples.append({
                    'component': 'OllamaEmbeddings',
                    'env_var': 'EMBEDDING_NAME',
                    'ui_display': model_value,
                    'code_line': line.strip()
                })
                break
        break

# 3. ChatInput 组件
for node in data['data']['nodes']:
    if 'ChatInput' in node['data']['id']:
        code = node['data']['node']['template']['code']['value']
        input_value = node['data']['node']['template']['input_value']['value']

        for line in code.split('\n'):
            if 'os.getenv("QA_SERVER_GET_QUESTION_URL"' in line:
                examples.append({
                    'component': 'ChatInput',
                    'env_var': 'QA_SERVER_GET_QUESTION_URL',
                    'ui_display': input_value,
                    'code_line': line.strip()
                })
                break
        break

for i, ex in enumerate(examples, 1):
    print(f"{i}. {ex['component']} 组件")
    print(f"   环境变量: {ex['env_var']}")
    print(f"   UI 显示值: '{ex['ui_display']}' (空或默认值)")
    print(f"   代码逻辑: {ex['code_line']}")
    print()

print("=" * 80)
print("验证方法")
print("=" * 80)

print("""
方法 1: 检查代码中的 os.getenv() 调用
-------------------------------------
""")

print("执行以下命令查看所有环境变量使用：\n")

# 生成验证命令
for node in data['data']['nodes']:
    node_id = node['data']['id']
    if any(x in node_id for x in ['Directory', 'Ollama', 'Qwen', 'MiniOB', 'ChatInput', 'TestOutput']):
        code = node['data']['node'].get('template', {}).get('code', {}).get('value', '')
        if 'os.getenv' in code:
            env_vars = []
            for line in code.split('\n'):
                if 'os.getenv("' in line:
                    start = line.find('os.getenv("') + 11
                    end = line.find('"', start)
                    if end > start:
                        env_var = line[start:end]
                        if env_var not in env_vars:
                            env_vars.append(env_var)

            if env_vars:
                print(f"  {node_id.split('-')[0]}: {', '.join(env_vars)}")

print("""
方法 2: 运行时查看日志
---------------------
运行工作流时，查看以下日志输出：

  [ChatInput] QA_SERVER_GET_QUESTION_URL: http://...
  [TestOutput] QA_SERVER_POST_ANSWER_URL: http://...
  [Directory] Using path: /path/to/docs
  [OllamaEmbeddings] Using model: bge-m3

如果环境变量未设置，会显示：
  [ChatInput] ERROR: QA_SERVER_GET_QUESTION_URL not set!

方法 3: 手动测试环境变量
-----------------------
在测评环境的 shell 中执行：

  echo $OB_DOC_PATH
  echo $EMBEDDING_NAME
  echo $QA_SERVER_GET_QUESTION_URL
  echo $QA_SERVER_POST_ANSWER_URL

如果输出为空，说明环境变量未设置。

方法 4: 检查 model.json 代码
--------------------------
使用本脚本的输出确认每个组件都有 os.getenv() 调用。
""")

print("=" * 80)
print("结论")
print("=" * 80)
print("""
✓ UI 输入框 **不应该** 显示环境变量占位符（如 $OB_DOC_PATH）
✓ UI 输入框显示 **默认值**（通常为空或预设值）
✓ 环境变量在 **代码运行时** 通过 os.getenv() 动态读取
✓ 如果看到输入框为空，这是 **正常现象**

验证是否正确配置：
- 查看代码中是否有 os.getenv("ENV_VAR", default) 调用
- 运行时查看日志输出
- 确认测评环境已设置所需的环境变量
""")
