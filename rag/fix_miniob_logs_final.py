import json
import re

# 读取JSON文件
with open('model.json', 'r', encoding='utf-8') as f:
    data = json.load(f)

# 找到MiniOB组件
miniob_node = None
for node in data.get('data', {}).get('nodes', []):
    if 'MiniOB' in node.get('data', {}).get('id', ''):
        miniob_node = node
        print(f"找到MiniOB节点: {node['data']['id']}")
        break

if not miniob_node:
    print("错误：找不到MiniOB节点！")
    exit(1)

# 获取code
code_value = miniob_node['data']['node']['template']['code']['value']

print(f"原始代码长度: {len(code_value)} 字符")
print(f"原始log_func出现次数: {code_value.count('log_func')}")

# 1. 移除build_vector_store中传递log_func的代码
code_value = re.sub(
    r',\s*log_func=self\.log',
    '',
    code_value,
    flags=re.MULTILINE
)

# 2. 移除所有 self.log_func( 单行调用
code_value = re.sub(
    r'\s*self\.log_func\([^)]*\)\s*\n',
    '',
    code_value,
    flags=re.MULTILINE
)

# 3. 移除所有 self.__log_func( 单行调用
code_value = re.sub(
    r'\s*self\.__log_func\([^)]*\)\s*\n',
    '',
    code_value,
    flags=re.MULTILINE
)

# 4. 移除 self.log( 调用
code_value = re.sub(
    r'\s*self\.log\([^)]*\)\s*\n',
    '',
    code_value,
    flags=re.MULTILINE
)

# 5. 移除多行的log调用 (带续行的)
code_value = re.sub(
    r'\s*self\.log_func\(\s*f?"[^"]*"\s*\)\s*\n',
    '',
    code_value,
    flags=re.MULTILINE | re.DOTALL
)

print(f"清理后代码长度: {len(code_value)} 字符")
print(f"清理后log_func出现次数: {code_value.count('log_func')}")

# 更新code
miniob_node['data']['node']['template']['code']['value'] = code_value

# 保存修改后的JSON
with open('model.json', 'w', encoding='utf-8') as f:
    json.dump(data, f, ensure_ascii=False, indent=2)

print("\n✓ 所有MiniOB日志输出已清理完成！")
print("✓ 修改已保存到model.json")
