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
        break

code = miniob_node['data']['node']['template']['code']['value']
print(f"Before: {code.count('log_func')} log_func occurrences")

# 方法1: 替换所有log_func调用为空函数
# 将 self.log_func = log_func or (lambda msg: None)
# 改为 self.log_func = lambda msg: None
code = re.sub(
    r'self\.log_func = log_func or \(lambda msg: None\)',
    'self.log_func = lambda msg: None',
    code
)

code = re.sub(
    r'self\.__log_func = log_func or \(lambda msg: None\)',
    'self.__log_func = lambda msg: None',
    code
)

# 方法2: 移除所有log_func的实际调用
# 匹配 self.log_func( 开始到对应的 ) 结束，支持嵌套
lines = code.split('\n')
new_lines = []
skip_line = False

for i, line in enumerate(lines):
    # 检查是否是log_func调用行
    if 'self.log_func(' in line or 'self.__log_func(' in line or 'self.log(' in line:
        # 计算括号是否平衡
        open_count = line.count('(')
        close_count = line.count(')')

        if open_count == close_count:
            # 单行调用，直接跳过
            continue
        else:
            # 多行调用开始
            skip_line = True
            temp_open = open_count
            temp_close = close_count

            # 查找结束行
            j = i + 1
            while j < len(lines) and temp_open > temp_close:
                next_line = lines[j]
                temp_open += next_line.count('(')
                temp_close += next_line.count(')')
                j += 1

            # 跳过从i到j的所有行
            if j < len(lines):
                # 标记要跳过的行
                for k in range(i, j):
                    lines[k] = '###SKIP###'
            continue

    if line == '###SKIP###':
        continue

    new_lines.append(line)

code = '\n'.join(new_lines)

print(f"After: {code.count('log_func')} log_func occurrences")

# 更新
miniob_node['data']['node']['template']['code']['value'] = code

# 保存
with open('model.json', 'w', encoding='utf-8') as f:
    json.dump(data, f, ensure_ascii=False, indent=2)

print("Cleanup completed!")
