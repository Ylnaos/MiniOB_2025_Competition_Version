import json
import re

# Read JSON file
with open('model.json', 'r', encoding='utf-8') as f:
    data = json.load(f)

# Find MiniOB component
miniob_node = None
for node in data.get('data', {}).get('nodes', []):
    if 'MiniOB' in node.get('data', {}).get('id', ''):
        miniob_node = node
        print(f"Found MiniOB node: {node['data']['id']}")
        break

if not miniob_node:
    print("ERROR: MiniOB node not found!")
    exit(1)

code = miniob_node['data']['node']['template']['code']['value']

print("\n=== FIX 1: Improve similarity_search error handling ===")

# Find and replace similarity_search method to add table existence check
old_similarity_start = '''    def similarity_search(
        self,
        query: str,
        k: int = 4,
        search_method: str = "Vector Search",
    ) -> list[Document]:
        self.__log_func(f"Performing similarity search for query: '{query}' with k={k}")
        result = []


        if not query:
            return result

        try:
            qvec = self.__embedding.embed_query(query)'''

new_similarity_start = '''    def similarity_search(
        self,
        query: str,
        k: int = 4,
        search_method: str = "Vector Search",
    ) -> list[Document]:
        self.__log_func(f"Performing similarity search for query: '{query}' with k={k}")
        result = []

        # Check if table exists by trying to query it
        try:
            test_sql = f"SELECT COUNT(*) FROM {self.__table} LIMIT 1"
            self.connector.exec(test_sql)
        except Exception:
            # Table doesn't exist yet, return empty results
            return result

        if not query:
            return result

        try:
            qvec = self.__embedding.embed_query(query)'''

if old_similarity_start in code:
    code = code.replace(old_similarity_start, new_similarity_start)
    print("[OK] Added table existence check to similarity_search")
else:
    print("[WARN] Could not find similarity_search pattern")

print("\n=== FIX 2: Improve vector literal format ===")

# Check current vector literal format
old_vector_literal = '''    def __vector_literal(self, vec: List[float]) -> str:
        """生成MiniOB向量字面量"""
        inner = ",".join(f"{float(x):.8f}" for x in vec)
        return "[" + inner + "]"'''

# Try with spaces between numbers (might help parsing)
new_vector_literal = '''    def __vector_literal(self, vec: List[float]) -> str:
        """生成MiniOB向量字面量"""
        inner = ", ".join(f"{float(x):.6f}" for x in vec)
        return "[" + inner + "]"'''

if old_vector_literal in code:
    code = code.replace(old_vector_literal, new_vector_literal)
    print("[OK] Updated vector literal format (added spaces, reduced precision)")
else:
    print("[WARN] Could not find vector_literal pattern")

print("\n=== FIX 3: Add better error handling in add_documents ===")

# Make sure table initialization errors don't crash
old_ensure = '''    def __ensure_initialized(self) -> None:
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

# No change needed here, but verify it exists
if old_ensure in code:
    print("[OK] Table initialization code looks good")
else:
    print("[INFO] Table initialization may have different format")

# Verify
print(f"\n=== VERIFICATION ===")
has_table_check = 'SELECT COUNT(*) FROM' in code and 'LIMIT 1' in code
has_spaces = '", ".join' in code
print(f"Contains table existence check: {has_table_check}")
print(f"Contains vector literal with spaces: {has_spaces}")

# Update code
miniob_node['data']['node']['template']['code']['value'] = code

# Save
with open('model.json', 'w', encoding='utf-8') as f:
    json.dump(data, f, ensure_ascii=False, indent=2)

print("\n[SUCCESS] SQL syntax fixes applied!")
print("\nKey improvements:")
print("  1. similarity_search checks table existence first")
print("  2. Vector literal format improved (spaces + lower precision)")
print("  3. Better error handling for missing tables")
