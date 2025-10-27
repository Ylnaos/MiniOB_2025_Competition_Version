#!/usr/bin/env python3
"""
修复model.json中缺少的数据库创建和使用语句
"""
import json

# 读取model.json
with open('model.json', encoding='utf-8') as f:
    model = json.load(f)

# 找到MiniOB组件
for node in model['data']['nodes']:
    if 'MiniOB' in node['data']['id']:
        code = node['data']['node']['template']['code']['value']

        # 定位__ensure_initialized方法
        old_ensure_init = '''def __ensure_initialized(self) -> None:
        """确保表和索引已创建"""
        try:
            _ = self.connector.exec(f"DESC {self.__table}")
        except Exception:
            # 建表：指定VECTOR维度
            create_sql = (
                f"CREATE TABLE {self.__table} ("
                f"content TEXT, "
                f"embedding VECTOR({self.__embedding_dimension})"
                f")"
            )
            self.__log_func(f"Creating table: {create_sql}")
            res = self.connector.exec(create_sql)
            self.__log_func(f"Create table result: {res}")'''

        new_ensure_init = '''def __ensure_initialized(self) -> None:
        """确保数据库、表和索引已创建"""
        # 1. 创建数据库
        try:
            db_sql = "CREATE DATABASE IF NOT EXISTS rag_db"
            self.__log_func(f"Creating database: {db_sql}")
            self.connector.exec(db_sql)
        except Exception as e:
            self.__log_func(f"Create database (may already exist): {e}")

        # 2. 使用数据库
        try:
            use_sql = "USE rag_db"
            self.__log_func(f"Using database: {use_sql}")
            self.connector.exec(use_sql)
        except Exception as e:
            self.__log_func(f"Use database failed: {e}")
            raise

        # 3. 检查表是否存在
        try:
            _ = self.connector.exec(f"DESC {self.__table}")
            self.__log_func(f"Table {self.__table} already exists")
        except Exception:
            # 建表：指定VECTOR维度
            create_sql = (
                f"CREATE TABLE {self.__table} ("
                f"content TEXT, "
                f"embedding VECTOR({self.__embedding_dimension})"
                f")"
            )
            self.__log_func(f"Creating table: {create_sql}")
            res = self.connector.exec(create_sql)
            self.__log_func(f"Create table result: {res}")'''

        # 替换
        if old_ensure_init in code:
            code = code.replace(old_ensure_init, new_ensure_init)
            print("[OK] Database initialization added")
        else:
            print("[ERROR] Could not find __ensure_initialized method to replace")
            print("Trying alternative replacement...")

            # 尝试更灵活的替换
            import re
            pattern = r'def __ensure_initialized\(self\) -> None:\s+"""[^"]+"""\s+try:\s+_ = self\.connector\.exec\(f"DESC \{self\.__table\}"\)'

            if re.search(pattern, code):
                print("[Found] Pattern match, will replace")
                # 手动构建替换
                code = re.sub(
                    pattern,
                    'def __ensure_initialized(self) -> None:\n        """确保数据库、表和索引已创建"""\n        # 1. 创建数据库\n        try:\n            db_sql = "CREATE DATABASE IF NOT EXISTS rag_db"\n            self.__log_func(f"Creating database: {db_sql}")\n            self.connector.exec(db_sql)\n        except Exception as e:\n            self.__log_func(f"Create database (may already exist): {e}")\n        \n        # 2. 使用数据库\n        try:\n            use_sql = "USE rag_db"\n            self.__log_func(f"Using database: {use_sql}")\n            self.connector.exec(use_sql)\n        except Exception as e:\n            self.__log_func(f"Use database failed: {e}")\n            raise\n        \n        # 3. 检查表是否存在\n        try:\n            _ = self.connector.exec(f"DESC {self.__table}")',
                    code
                )
                print("[OK] Replacement done via regex")

        # 更新
        node['data']['node']['template']['code']['value'] = code
        print(f"Code length: {len(code)}")
        break

# 保存
with open('model.json', 'w', encoding='utf-8') as f:
    json.dump(model, f, indent=2, ensure_ascii=False)

print("\nVerifying...")

# 验证
with open('model.json', encoding='utf-8') as f:
    verify_model = json.load(f)

for node in verify_model['data']['nodes']:
    if 'MiniOB' in node['data']['id']:
        verify_code = node['data']['node']['template']['code']['value']

        if 'CREATE DATABASE' in verify_code:
            print("[OK] CREATE DATABASE statement added")
        if 'USE rag_db' in verify_code:
            print("[OK] USE database statement added")
        if 'DESC {self.__table}' in verify_code:
            print("[OK] Table existence check preserved")

        break

print("\nDone!")
