#!/usr/bin/env python3
"""
添加详细的调试日志到 TestOutput 组件
"""
import json

json_file = r"D:\迅雷下载\MiniOB2025\MiniOB_2025_Competition_Version\rag\model.json"

with open(json_file, "r", encoding="utf-8") as f:
    data = json.load(f)

# 找到 TestOutput 组件
for node in data["data"]["nodes"]:
    if node["data"]["id"] == "TestOutput-jW9PW":
        # 完整替换整个代码，添加详细日志
        new_code = '''# from langflow.field_typing import Data
from langflow.custom.custom_component.component import Component
from langflow.io import MessageTextInput, Output
from langflow.schema.data import Data
import requests
import os


class CustomComponent(Component):
    display_name = "Test Output"
    description = "Use as a template to create your own component."
    documentation: str = "https://docs.langflow.org/components-custom-components"
    icon = "code"
    name = "TestOutput"

    inputs = [
        MessageTextInput(
            name="input_value",
            display_name="Input Value",
            info="This is a custom component Input",
            value="Hello, World!",
            tool_mode=True,
        ),
    ]

    outputs = [
        Output(display_name="Output", name="output", method="build_output"),
    ]

    def build_output(self) -> Data:
        print("=" * 60)
        print("[TestOutput] Starting build_output")
        print(f"[TestOutput] input_value type: {type(self.input_value)}")
        print(f"[TestOutput] input_value repr: {repr(self.input_value)[:200]}")

        # 提取 Message 对象的文本内容
        answer_text = None

        # 检查各种可能的属性
        if hasattr(self.input_value, 'text'):
            answer_text = self.input_value.text
            print(f"[TestOutput] Extracted from .text attribute")
        elif hasattr(self.input_value, 'content'):
            answer_text = self.input_value.content
            print(f"[TestOutput] Extracted from .content attribute")
        elif isinstance(self.input_value, str):
            answer_text = self.input_value
            print(f"[TestOutput] Already a string")
        elif isinstance(self.input_value, dict) and 'text' in self.input_value:
            answer_text = self.input_value['text']
            print(f"[TestOutput] Extracted from dict['text']")
        else:
            answer_text = str(self.input_value)
            print(f"[TestOutput] Converted to string using str()")

        print(f"[TestOutput] Extracted text type: {type(answer_text)}")
        print(f"[TestOutput] Extracted text length: {len(str(answer_text))}")
        print(f"[TestOutput] Extracted text preview: {str(answer_text)[:200]}")

        data = Data(value=answer_text)
        self.status = data

        url = os.getenv("QA_SERVER_POST_ANSWER_URL")
        print(f"[TestOutput] QA_SERVER_POST_ANSWER_URL: {url}")

        if not url:
            print("[TestOutput] ERROR: QA_SERVER_POST_ANSWER_URL not set, skipping submission")
            return data

        headers = {
            "Content-Type": "application/json"
        }
        payload = {
            "answer": answer_text
        }

        print(f"[TestOutput] Payload preview: {str(payload)[:300]}")

        try:
            print(f"[TestOutput] Sending POST request to {url}")
            response = requests.post(url, json=payload, headers=headers, timeout=10)
            print(f"[TestOutput] Response status code: {response.status_code}")
            print(f"[TestOutput] Response headers: {dict(response.headers)}")
            print(f"[TestOutput] Response text: {response.text[:500]}")

            if response.status_code != 200:
                print(f"[TestOutput] WARNING: Non-200 status code!")
        except Exception as e:
            print(f"[TestOutput] ERROR: Failed to submit answer: {e}")
            import traceback
            traceback.print_exc()

        print("=" * 60)
        return data
'''

        node["data"]["node"]["template"]["code"]["value"] = new_code
        print("[OK] Added detailed debug logging to TestOutput")

        # 保存
        with open(json_file, "w", encoding="utf-8") as out_f:
            json.dump(data, out_f, indent=2, ensure_ascii=False)
        print(f"[OK] Saved to {json_file}")
        break

print("\nDone!")
