import json

# 组件ID映射
components = {
    "Directory": "Directory-RSP7d",
    "SplitText": "SplitText-Tu2iV",
    "OllamaEmbeddings": "OllamaEmbeddings-vt2mP",
    "ChatInput": "ChatInput-VGZSq",
    "MiniOB": "MiniOB-lrJi1",
    "parser": "parser-tiD4c",
    "Prompt": "Prompt-cjybM",
    "ChatQwenModel": "ChatQwenModel-5mSki",
    "ChatOutput": "ChatOutput-k7ET8",
    "TestOutput": "TestOutput-gbo7V",
}

# 定义连线关系
connections = [
    # 1. Directory → SplitText (文档加载)
    {
        "source": "Directory",
        "source_handle": {"dataType": "Directory", "id": components["Directory"], "name": "data", "output_types": ["Data"]},
        "target": "SplitText",
        "target_handle": {"fieldName": "data_inputs", "id": components["SplitText"], "inputTypes": ["Data"], "type": "other"}
    },
    # 2. SplitText → MiniOB (文档输入)
    {
        "source": "SplitText",
        "source_handle": {"dataType": "SplitText", "id": components["SplitText"], "name": "chunks", "output_types": ["Data"]},
        "target": "MiniOB",
        "target_handle": {"fieldName": "ingest_data", "id": components["MiniOB"], "inputTypes": ["Data"], "type": "other"}
    },
    # 3. OllamaEmbeddings → MiniOB (embedding模型)
    {
        "source": "OllamaEmbeddings",
        "source_handle": {"dataType": "OllamaEmbeddings", "id": components["OllamaEmbeddings"], "name": "embeddings", "output_types": ["Embeddings"]},
        "target": "MiniOB",
        "target_handle": {"fieldName": "embedding_model", "id": components["MiniOB"], "inputTypes": ["Embeddings"], "type": "other"}
    },
    # 4. ChatInput → MiniOB (搜索查询)
    {
        "source": "ChatInput",
        "source_handle": {"dataType": "ChatInput", "id": components["ChatInput"], "name": "message", "output_types": ["Message"]},
        "target": "MiniOB",
        "target_handle": {"fieldName": "search_query", "id": components["MiniOB"], "inputTypes": ["Message"], "type": "str"}
    },
    # 5. ChatInput → Prompt (用户问题)
    {
        "source": "ChatInput",
        "source_handle": {"dataType": "ChatInput", "id": components["ChatInput"], "name": "message", "output_types": ["Message"]},
        "target": "Prompt",
        "target_handle": {"fieldName": "question", "id": components["Prompt"], "inputTypes": ["Message", "Text"], "type": "str"}
    },
    # 6. MiniOB → parser (检索结果)
    {
        "source": "MiniOB",
        "source_handle": {"dataType": "MiniOB", "id": components["MiniOB"], "name": "search_results", "output_types": ["Data"]},
        "target": "parser",
        "target_handle": {"fieldName": "text", "id": components["parser"], "inputTypes": ["Data"], "type": "other"}
    },
    # 7. parser → Prompt (上下文)
    {
        "source": "parser",
        "source_handle": {"dataType": "parser", "id": components["parser"], "name": "parsed_text", "output_types": ["Message"]},
        "target": "Prompt",
        "target_handle": {"fieldName": "context", "id": components["Prompt"], "inputTypes": ["Message", "Text"], "type": "str"}
    },
    # 8. Prompt → ChatQwenModel (提示词)
    {
        "source": "Prompt",
        "source_handle": {"dataType": "Prompt", "id": components["Prompt"], "name": "prompt", "output_types": ["Message"]},
        "target": "ChatQwenModel",
        "target_handle": {"fieldName": "input_value", "id": components["ChatQwenModel"], "inputTypes": ["Message"], "type": "str"}
    },
    # 9. ChatQwenModel → ChatOutput (LLM响应)
    {
        "source": "ChatQwenModel",
        "source_handle": {"dataType": "ChatQwenModel", "id": components["ChatQwenModel"], "name": "text_output", "output_types": ["Message"]},
        "target": "ChatOutput",
        "target_handle": {"fieldName": "input_value", "id": components["ChatOutput"], "inputTypes": ["Message"], "type": "str"}
    },
    # 10. ChatOutput → TestOutput (最终输出)
    {
        "source": "ChatOutput",
        "source_handle": {"dataType": "ChatOutput", "id": components["ChatOutput"], "name": "message", "output_types": ["Message"]},
        "target": "TestOutput",
        "target_handle": {"fieldName": "answer", "id": components["TestOutput"], "inputTypes": ["Message"], "type": "str"}
    },
]

# 构建edges数组
edges = []
for conn in connections:
    source_handle_str = json.dumps(conn["source_handle"], separators=(',', ':'), ensure_ascii=False)
    target_handle_str = json.dumps(conn["target_handle"], separators=(',', ':'), ensure_ascii=False)

    # 生成edge ID（使用引号而不是特殊字符）
    edge_id = f"reactflow__edge-{conn['source']}{source_handle_str}-{conn['target']}{target_handle_str}"

    edge = {
        "animated": False,
        "className": "",
        "data": {
            "sourceHandle": conn["source_handle"],
            "targetHandle": conn["target_handle"]
        },
        "id": edge_id,
        "selected": False,
        "source": conn["source_handle"]["id"],
        "sourceHandle": source_handle_str,
        "target": conn["target_handle"]["id"],
        "targetHandle": target_handle_str
    }
    edges.append(edge)

# 输出结果
print(f"Generated {len(edges)} edges:")
for i, edge in enumerate(edges, 1):
    print(f"{i}. {edge['source'].split('-')[0]} → {edge['target'].split('-')[0]}")

# 保存到文件
with open('edges_output.json', 'w', encoding='utf-8') as f:
    json.dump(edges, f, indent=2, ensure_ascii=False)

print("\nEdges saved to edges_output.json")
