#!/usr/bin/env python3
"""
移除 MiniOB 组件中的 USE database 语句
直接在默认数据库中创建表
"""
import json

json_file = r"D:\迅雷下载\MiniOB2025\MiniOB_2025_Competition_Version\rag\model.json"

with open(json_file, "r", encoding="utf-8") as f:
    data = json.load(f)

# 找到 MiniOB 组件
for node in data["data"]["nodes"]:
    if node["data"]["id"] == "MiniOB-f9CHM":
        code_value = node["data"]["node"]["template"]["code"]["value"]

        # 定义新的 __ensure_initialized 方法（移除数据库创建和 USE 语句）
        old_method = '''    def __ensure_initialized(self) -> None:
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
            self.__log_func(f"Create table result: {res}")

        # 创建向量索引
        try:
            index_sql = (
                f"CREATE VECTOR INDEX {self.__index} "
                f"ON {self.__table} (embedding) "
                f"WITH (TYPE=IVFFLAT, DISTANCE=COSINE_DISTANCE, LISTS=64, PROBES=8)"
            )
            self.__log_func(f"Ensuring vector index: {index_sql}")
            _ = self.connector.exec(index_sql)
        except Exception as e:
            self.__log_func(f"Create vector index ignored: {e}")'''

        new_method = '''    def __ensure_initialized(self) -> None:
        """确保表和索引已创建（在默认数据库中）"""
        # 检查表是否存在
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
            self.__log_func(f"Create table result: {res}")

        # 创建向量索引
        try:
            index_sql = (
                f"CREATE VECTOR INDEX {self.__index} "
                f"ON {self.__table} (embedding) "
                f"WITH (TYPE=IVFFLAT, DISTANCE=COSINE_DISTANCE, LISTS=64, PROBES=8)"
            )
            self.__log_func(f"Ensuring vector index: {index_sql}")
            _ = self.connector.exec(index_sql)
        except Exception as e:
            self.__log_func(f"Create vector index ignored: {e}")'''

        if old_method in code_value:
            code_value = code_value.replace(old_method, new_method)
            node["data"]["node"]["template"]["code"]["value"] = code_value
            print("[OK] Removed USE database statement")

            # 保存修改后的文件
            with open(json_file, "w", encoding="utf-8") as out_f:
                json.dump(data, out_f, indent=2, ensure_ascii=False)
            print(f"[OK] Saved changes to {json_file}")
        else:
            print("[WARNING] Target code not found, may already be modified")
            print("Searching for possible matches...")
            if "USE rag_db" in code_value:
                print("[ERROR] Code still contains 'USE rag_db', manual check needed")
            else:
                print("[OK] Code does not contain 'USE rag_db'")

print("\nDone!")
