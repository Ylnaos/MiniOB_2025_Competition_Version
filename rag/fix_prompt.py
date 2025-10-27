#!/usr/bin/env python3
"""
优化 Prompt 模板，确保 LLM 回答包含关键术语
"""
import json

json_file = r"D:\迅雷下载\MiniOB2025\MiniOB_2025_Competition_Version\rag\model.json"

with open(json_file, "r", encoding="utf-8") as f:
    data = json.load(f)

# 新的 Prompt 模板
new_template = '''以下是从文档中检索到的相关内容：

{context}

---

请基于上述检索结果回答问题。**重要要求**：

1. 回答必须**直接引用**检索结果中的关键术语、章节标题和专业名词
2. 回答必须**详细具体**，包含检索结果中的数值、限制条件等细节
3. 如果检索结果中包含表格、列表等结构化信息，请在回答中体现
4. 保持检索结果中的**原文表述**，不要过度改写或总结
5. 如果检索结果不足以回答问题，明确说明

问题：{question}

回答：'''

# 找到 Prompt 组件并更新
for node in data["data"]["nodes"]:
    if "Prompt" in node["data"]["id"]:
        template_field = node["data"]["node"]["template"]["template"]
        old_template = template_field["value"]

        print("=== 旧 Prompt 模板 ===")
        print(old_template)
        print("\n=== 新 Prompt 模板 ===")
        print(new_template)

        # 更新模板
        template_field["value"] = new_template

        print("\n[OK] Prompt 模板已优化")

        # 保存
        with open(json_file, "w", encoding="utf-8") as out_f:
            json.dump(data, out_f, indent=2, ensure_ascii=False)
        print(f"[OK] 已保存到 {json_file}")
        break

print("\nDone!")
