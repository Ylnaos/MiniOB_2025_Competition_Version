import json
import re

# 读取JSON文件
with open('model.json', 'r', encoding='utf-8') as f:
    data = json.load(f)

# 找到MiniOB组件并修改code
for node in data.get('data', {}).get('nodes', []):
    if node.get('data', {}).get('id') == 'MiniOB-J73jv':
        code_value = node['data']['node']['template']['code']['value']

        # 移除所有 self.log_func( 调用
        # 将 self.log_func(xxx) 替换为空语句 pass
        code_value = re.sub(
            r'self\.log_func\([^)]*\)',
            'pass  # logging disabled',
            code_value
        )

        # 移除所有 self.__log_func( 调用
        code_value = re.sub(
            r'self\.__log_func\([^)]*\)',
            'pass  # logging disabled',
            code_value
        )

        # 移除log_func参数传递
        code_value = re.sub(
            r',\s*log_func=self\.log',
            '',
            code_value
        )

        # 移除 self.log( 调用
        code_value = re.sub(
            r'self\.log\([^)]*\)',
            'pass  # logging disabled',
            code_value
        )

        # 更新code
        node['data']['node']['template']['code']['value'] = code_value
        print(f"MiniOB组件日志清理完成")

        # 统计修改次数
        pass_count = code_value.count('# logging disabled')
        print(f"共清理了 {pass_count} 处日志输出")
        break

# 保存修改后的JSON
with open('model.json', 'w', encoding='utf-8') as f:
    json.dump(data, f, ensure_ascii=False, indent=2)

print("所有修改已保存到model.json")
