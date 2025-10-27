#!/usr/bin/env python3
"""
修复 __ensure_initialized 方法
直接尝试 CREATE TABLE，不依赖 DESC 的返回结果
"""
import json

json_file = r"D:\迅雷下载\MiniOB2025\MiniOB_2025_Competition_Version\rag\model.json"

with open(json_file, "r", encoding="utf-8") as f:
    data = json.load(f)

# 找到 MiniOB 组件
for node in data["data"]["nodes"]:
    if node["data"]["id"] == "MiniOB-f9CHM":
        code_value = node["data"]["node"]["template"]["code"]["value"]

        # 旧的错误实现
        old_method = '''    def __ensure_initialized(self) -> None:
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

        # 新的正确实现：直接尝试创建表，不依赖 DESC
        new_method = '''    def __ensure_initialized(self) -> None:
        """确保表和索引已创建（在默认数据库中）"""
        # 直接尝试建表（如果已存在会失败，忽略即可）
        create_sql = (
            f"CREATE TABLE {self.__table} ("
            f"content TEXT, "
            f"embedding VECTOR({self.__embedding_dimension})"
            f")"
        )
        self.__log_func(f"Attempting to create table: {create_sql}")
        try:
            res = self.connector.exec(create_sql)
            self.__log_func(f"Create table succeeded: {res}")
        except Exception as e:
            self.__log_func(f"Create table skipped (may already exist): {e}")

        # 创建向量索引（如果已存在会失败，忽略即可）
        index_sql = (
            f"CREATE VECTOR INDEX {self.__index} "
            f"ON {self.__table} (embedding) "
            f"WITH (TYPE=IVFFLAT, DISTANCE=COSINE_DISTANCE, LISTS=64, PROBES=8)"
        )
        self.__log_func(f"Attempting to create vector index: {index_sql}")
        try:
            res = self.connector.exec(index_sql)
            self.__log_func(f"Create vector index succeeded: {res}")
        except Exception as e:
            self.__log_func(f"Create vector index skipped (may already exist): {e}")'''

        if old_method in code_value:
            code_value = code_value.replace(old_method, new_method)
            node["data"]["node"]["template"]["code"]["value"] = code_value
            print("[OK] Fixed __ensure_initialized method")

            # 保存
            with open(json_file, "w", encoding="utf-8") as out_f:
                json.dump(data, out_f, indent=2, ensure_ascii=False)
            print(f"[OK] Saved to {json_file}")
        else:
            print("[ERROR] Could not find old method to replace")
            if "__ensure_initialized" in code_value:
                print("[INFO] Method exists but signature doesn't match")
            else:
                print("[ERROR] Method __ensure_initialized not found at all")

print("\nDone!")
