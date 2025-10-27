#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""修复model.json中的MiniOB组件初始化代码"""

import json
import sys
import io

# 设置stdout为UTF-8编码
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

def main():
    json_file = 'rag/model.json'

    # 读取JSON
    with open(json_file, 'r', encoding='utf-8') as f:
        data = json.load(f)

    # 找到MiniOB组件
    modified = False
    for node in data['data']['nodes']:
        if 'MiniOB' in node['data'].get('display_name', ''):
            code = node['data']['node']['template']['code']['value']

            # 查找并替换__ensure_initialized方法
            marker = '    def __ensure_initialized(self) -> None:'
            if marker in code:
                # 找到方法的开始位置
                start_idx = code.find(marker)

                # 找到方法的结束位置（下一个方法定义或类定义）
                end_markers = ['\n    def ', '\n    @property', '\nclass ']
                end_idx = len(code)
                for end_marker in end_markers:
                    temp_idx = code.find(end_marker, start_idx + len(marker))
                    if temp_idx != -1 and temp_idx < end_idx:
                        end_idx = temp_idx

                # 新的方法实现
                new_method = '''    def __ensure_initialized(self) -> None:
        # 检查表是否存在：若不存在则创建；随后创建/确保存在向量索引
        self.__log_func(f"Checking if table {self.__table} exists...")
        table_exists = False
        try:
            desc_result = self.connector.exec(f"DESC {self.__table}")
            self.__log_func(f"Table {self.__table} exists. DESC result: {desc_result}")
            table_exists = True
        except Exception as e:
            self.__log_func(f"Table {self.__table} does not exist. Error: {e}")
            # 建表：VECTOR 不带维度（解析器内部默认维度）
            create_sql = (
                f"CREATE TABLE {self.__table} ("
                f"content TEXT, "
                f"embedding VECTOR"
                f")"
            )
            self.__log_func(f"Creating table with SQL: {create_sql}")
            try:
                res = self.connector.exec(create_sql)
                self.__log_func(f"Create table SUCCESS. Result: {res}")
                table_exists = True
            except Exception as create_err:
                self.__log_func(f"Create table FAILED: {create_err}")
                raise MiniOBException(
                    message=f"Failed to create table {self.__table}: {create_err}",
                    operation="CREATE_TABLE",
                    details={"sql": create_sql}
                ) from create_err

        if not table_exists:
            raise MiniOBException(
                message=f"Table {self.__table} is not ready",
                operation="TABLE_INITIALIZATION"
            )

        # 创建向量索引（若未存在）
        try:
            index_sql = (
                f"CREATE VECTOR INDEX {self.__index} "
                f"ON {self.__table} {{ embedding }} "
                f"WITH {{ TYPE = IVFFLAT, DISTANCE = COSINE_DISTANCE, LISTS = 64, PROBES = 8 }}"
            )
            self.__log_func(f"Ensuring vector index with SQL: {index_sql}")
            idx_result = self.connector.exec(index_sql)
            self.__log_func(f"Create vector index result: {idx_result}")
        except Exception as e:
            # 索引已存在或语法不支持时，静默忽略
            self.__log_func(f"Create vector index ignored (may already exist): {e}")

'''

                # 替换方法
                new_code = code[:start_idx] + new_method + code[end_idx:]

                # 更新节点
                node['data']['node']['template']['code']['value'] = new_code
                modified = True
                print(f"[OK] Modified MiniOB component - __ensure_initialized method updated")
                print(f"  Old method length: {end_idx - start_idx} characters")
                print(f"  New method length: {len(new_method)} characters")
                break
            else:
                print(f"[ERROR] Could not find {marker} in MiniOB component code")
                return 1

    if not modified:
        print("[ERROR] MiniOB component not found in model.json")
        return 1

    # 保存修改后的JSON
    with open(json_file, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)

    print(f"\n[OK] Successfully updated {json_file}")
    return 0

if __name__ == '__main__':
    sys.exit(main())
