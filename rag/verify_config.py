#!/usr/bin/env python3
"""
验证所有组件是否正确配置了环境变量
"""
import json

json_file = r"D:\迅雷下载\MiniOB2025\MiniOB_2025_Competition_Version\rag\model.json"

with open(json_file, "r", encoding="utf-8") as f:
    data = json.load(f)

print("=" * 70)
print("环境变量配置检查")
print("=" * 70)

checks = []

# 1. Directory 组件
for node in data["data"]["nodes"]:
    if "Directory" in node["data"]["id"]:
        code = node["data"]["node"]["template"]["code"]["value"]
        has_env = 'os.getenv("OB_DOC_PATH"' in code
        checks.append(("Directory", "OB_DOC_PATH", has_env))
        print(f"[{'OK' if has_env else 'ERROR'}] Directory 组件使用 OB_DOC_PATH: {has_env}")
        break

# 2. Ollama Embeddings 组件
for node in data["data"]["nodes"]:
    if "OllamaEmbeddings" in node["data"]["id"]:
        code = node["data"]["node"]["template"]["code"]["value"]
        has_name = 'os.getenv("EMBEDDING_NAME"' in code
        has_url = 'os.getenv("EMBEDDING_BASE_URL"' in code
        checks.append(("OllamaEmbeddings", "EMBEDDING_NAME", has_name))
        checks.append(("OllamaEmbeddings", "EMBEDDING_BASE_URL", has_url))
        print(f"[{'OK' if has_name else 'ERROR'}] OllamaEmbeddings 使用 EMBEDDING_NAME: {has_name}")
        print(f"[{'OK' if has_url else 'ERROR'}] OllamaEmbeddings 使用 EMBEDDING_BASE_URL: {has_url}")
        break

# 3. Qwen 组件
for node in data["data"]["nodes"]:
    if "Qwen" in node["data"]["id"]:
        code = node["data"]["node"]["template"]["code"]["value"]
        has_name = 'os.getenv("LLM_NAME"' in code
        has_key = 'os.getenv("LLM_API_KEY"' in code
        has_url = 'os.getenv("LLM_BASE_URL"' in code
        checks.append(("Qwen", "LLM_NAME", has_name))
        checks.append(("Qwen", "LLM_API_KEY", has_key))
        checks.append(("Qwen", "LLM_BASE_URL", has_url))
        print(f"[{'OK' if has_name else 'ERROR'}] Qwen 使用 LLM_NAME: {has_name}")
        print(f"[{'OK' if has_key else 'ERROR'}] Qwen 使用 LLM_API_KEY: {has_key}")
        print(f"[{'OK' if has_url else 'ERROR'}] Qwen 使用 LLM_BASE_URL: {has_url}")
        break

# 4. MiniOB 组件
for node in data["data"]["nodes"]:
    if "MiniOB" in node["data"]["id"]:
        code = node["data"]["node"]["template"]["code"]["value"]
        has_socket = 'os.getenv("MINIOB_SERVER_SOCKET"' in code
        checks.append(("MiniOB", "MINIOB_SERVER_SOCKET", has_socket))
        print(f"[{'OK' if has_socket else 'ERROR'}] MiniOB 使用 MINIOB_SERVER_SOCKET: {has_socket}")
        break

# 5. ChatInput 组件
for node in data["data"]["nodes"]:
    if "ChatInput" in node["data"]["id"]:
        code = node["data"]["node"]["template"]["code"]["value"]
        has_get = 'os.getenv("QA_SERVER_GET_QUESTION_URL"' in code
        checks.append(("ChatInput", "QA_SERVER_GET_QUESTION_URL", has_get))
        print(f"[{'OK' if has_get else 'ERROR'}] ChatInput 使用 QA_SERVER_GET_QUESTION_URL: {has_get}")
        break

# 6. TestOutput 组件
for node in data["data"]["nodes"]:
    if "TestOutput" in node["data"]["id"]:
        code = node["data"]["node"]["template"]["code"]["value"]
        has_post = 'os.getenv("QA_SERVER_POST_ANSWER_URL"' in code
        checks.append(("TestOutput", "QA_SERVER_POST_ANSWER_URL", has_post))
        print(f"[{'OK' if has_post else 'ERROR'}] TestOutput 使用 QA_SERVER_POST_ANSWER_URL: {has_post}")
        break

print("\n" + "=" * 70)
all_ok = all(check[2] for check in checks)
if all_ok:
    print("结论: 所有环境变量配置正确！")
    print("\n如果仍然评分为0，可能的原因:")
    print("1. 测评环境中环境变量未设置")
    print("2. 回答质量问题 - 没有包含 support_facts 中的关键词")
    print("3. 工作流执行出错（查看详细日志）")
else:
    print("结论: 发现配置问题，需要修复")
print("=" * 70)
