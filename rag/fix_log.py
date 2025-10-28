import json
import re

# 读取JSON文件
with open('model.json', 'r', encoding='utf-8') as f:
    data = json.load(f)

# 找到MiniOB组件并修改code
for node in data.get('data', {}).get('nodes', []):
    if node.get('data', {}).get('id') == 'MiniOB-J73jv':
        code_value = node['data']['node']['template']['code']['value']

        # 移除build_vector_store方法中的log_func=self.log传递
        code_value = re.sub(
            r'log_func=self\.log,',
            '',
            code_value
        )

        # 更新code
        node['data']['node']['template']['code']['value'] = code_value
        print("已移除MiniOB组件中的log_func传递")
        break

# 保存修改后的JSON
with open('model.json', 'w', encoding='utf-8') as f:
    json.dump(data, f, ensure_ascii=False, indent=2)

print("修改已保存")
