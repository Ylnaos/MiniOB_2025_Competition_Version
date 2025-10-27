#!/usr/bin/env python3
"""
修复TEXT类型导致的FILE_NOT_EXIST错误
方案1: 将TEXT改为VARCHAR(10000)
方案2: 在CREATE TABLE后添加SYNC命令
"""

import json
import sys

def main():
    model_json_path = "D:\\迅雷下载\\MiniOB2025\\MiniOB_2025_Competition_Version\\rag\\model.json"

    print(f"Reading {model_json_path}...")
    with open(model_json_path, 'r', encoding='utf-8') as f:
        data = json.load(f)

    # Find MiniOB node
    found = False
    for node in data.get('data', {}).get('nodes', []):
        if 'MiniOB' in node.get('data', {}).get('type', ''):
            print("Found MiniOB node, applying fixes...")
            code = node['data']['node']['template']['code']['value']

            # Fix 1: TEXT -> VARCHAR(10000)
            if 'content TEXT' in code:
                code = code.replace('content TEXT', 'content VARCHAR(10000)')
                print("[OK] Replaced TEXT with VARCHAR(10000)")

            # Fix 2: Add SYNC after CREATE TABLE
            if 'self.connector.exec(create_table_sql)' in code:
                code = code.replace(
                    'self.connector.exec(create_table_sql)\n            self.__log_func("Table created successfully")',
                    'self.connector.exec(create_table_sql)\n            self.__log_func("Table created successfully")\n            \n            # Sync to ensure table files are created\n            try:\n                self.connector.exec("SYNC")\n                self.__log_func("SYNC executed successfully")\n            except Exception as sync_err:\n                self.__log_func(f"SYNC warning: {str(sync_err)}")'
                )
                print("[OK] Added SYNC after CREATE TABLE")

            # Fix 3: Add SYNC after CREATE INDEX
            if 'self.connector.exec(create_index_sql)' in code and 'Vector index created' in code:
                # Find the line after index creation
                lines = code.split('\n')
                new_lines = []
                for i, line in enumerate(lines):
                    new_lines.append(line)
                    if 'self.__log_func("Vector index created successfully")' in line:
                        # Add SYNC after this line
                        indent = ' ' * (len(line) - len(line.lstrip()))
                        new_lines.append(f'{indent}try:')
                        new_lines.append(f'{indent}    self.connector.exec("SYNC")')
                        new_lines.append(f'{indent}    self.__log_func("SYNC after index creation")')
                        new_lines.append(f'{indent}except Exception as sync_err:')
                        new_lines.append(f'{indent}    self.__log_func(f"SYNC warning: {{str(sync_err)}}")')
                code = '\n'.join(new_lines)
                print("[OK] Added SYNC after CREATE INDEX")

            # Update code
            node['data']['node']['template']['code']['value'] = code
            found = True
            break

    if not found:
        print("ERROR: MiniOB node not found!")
        sys.exit(1)

    # Write back
    print(f"Writing back to {model_json_path}...")
    with open(model_json_path, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)

    print("[OK] All fixes applied successfully!")
    print("\nFixes applied:")
    print("1. TEXT -> VARCHAR(10000) to avoid LOB file issues")
    print("2. Added SYNC after CREATE TABLE to ensure files are created")
    print("3. Added SYNC after CREATE INDEX to ensure index is ready")

if __name__ == '__main__':
    main()
