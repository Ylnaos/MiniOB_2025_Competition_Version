#!/usr/bin/env python3
"""
修复 TestOutput 组件代码
正确提取 Message 对象的文本内容
"""
import json

json_file = r"D:\迅雷下载\MiniOB2025\MiniOB_2025_Competition_Version\rag\model.json"

with open(json_file, "r", encoding="utf-8") as f:
    data = json.load(f)

# 找到 TestOutput 组件
for node in data["data"]["nodes"]:
    if node["data"]["id"] == "TestOutput-jW9PW":
        # 完整替换整个代码
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
        # 提取 Message 对象的文本内容
        answer_text = self.input_value
        if hasattr(self.input_value, 'text'):
            answer_text = self.input_value.text
        elif hasattr(self.input_value, 'content'):
            answer_text = self.input_value.content
        else:
            answer_text = str(self.input_value)

        data = Data(value=answer_text)
        self.status = data

        url = os.getenv("QA_SERVER_POST_ANSWER_URL")
        if not url:
            print("[TestOutput] QA_SERVER_POST_ANSWER_URL not set, skipping submission")
            return data

        headers = {
            "Content-Type": "application/json"
        }
        payload = {
            "answer": answer_text
        }

        try:
            response = requests.post(url, json=payload, headers=headers, timeout=10)
            print(f"[TestOutput] Submitted answer to {url}, status: {response.status_code}")
            if response.status_code != 200:
                print(f"[TestOutput] Server response: {response.text}")
        except Exception as e:
            print(f"[TestOutput] Failed to submit answer: {e}")

        return data
'''

        node["data"]["node"]["template"]["code"]["value"] = new_code
        print("[OK] Replaced TestOutput code completely")

        # 保存
        with open(json_file, "w", encoding="utf-8") as out_f:
            json.dump(data, out_f, indent=2, ensure_ascii=False)
        print(f"[OK] Saved to {json_file}")
        break

print("\nDone!")
