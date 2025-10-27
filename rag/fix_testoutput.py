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
        code_value = node["data"]["node"]["template"]["code"]["value"]

        # 旧的错误代码
        old_code = '''    def build_output(self) -> Data:
        data = Data(value=self.input_value)
        self.status = data

        url = os.getenv("QA_SERVER_POST_ANSWER_URL")
        headers = {
            "Content-Type": "application/json"
        }
        data = {
            "answer": self.input_value
        }

        requests.post(url, json=data, headers=headers)
        return data'''

        # 新的正确代码
        new_code = '''    def build_output(self) -> Data:
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
            self.log("QA_SERVER_POST_ANSWER_URL not set, skipping submission")
            return data

        headers = {
            "Content-Type": "application/json"
        }
        payload = {
            "answer": answer_text
        }

        try:
            response = requests.post(url, json=payload, headers=headers, timeout=10)
            self.log(f"Submitted answer to {url}, status: {response.status_code}")
            if response.status_code != 200:
                self.log(f"Server response: {response.text}")
        except Exception as e:
            self.log(f"Failed to submit answer: {e}")

        return data'''

        if old_code in code_value:
            code_value = code_value.replace(old_code, new_code)
            node["data"]["node"]["template"]["code"]["value"] = code_value
            print("[OK] Fixed TestOutput component")

            # 保存
            with open(json_file, "w", encoding="utf-8") as out_f:
                json.dump(data, out_f, indent=2, ensure_ascii=False)
            print(f"[OK] Saved to {json_file}")
        else:
            print("[ERROR] Could not find old code to replace")
            print("\nSearching for build_output method...")
            if "def build_output" in code_value:
                print("[INFO] Method exists but signature doesn't match exactly")
                # 尝试更宽松的匹配
                if "self.input_value" in code_value and '"answer": self.input_value' in code_value:
                    print("[WARNING] Found the bug pattern, but exact match failed")
                    print("Manual inspection needed")
            else:
                print("[ERROR] Method build_output not found at all")

print("\nDone!")
