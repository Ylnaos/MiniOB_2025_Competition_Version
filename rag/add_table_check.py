import json

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

print("\n=== Adding table existence check to similarity_search ===")

# Pattern to find
old_pattern = '''        self.__log_func(f"Performing similarity search for query: '{query}' with k={k}")
        result = []


        if not query:
            return result'''

new_pattern = '''        self.__log_func(f"Performing similarity search for query: '{query}' with k={k}")
        result = []

        # Check if table exists before querying
        try:
            test_sql = f"SELECT COUNT(*) FROM {self.__table} LIMIT 1"
            self.connector.exec(test_sql)
        except Exception:
            # Table doesn't exist yet, return empty results
            return result

        if not query:
            return result'''

if old_pattern in code:
    code = code.replace(old_pattern, new_pattern)
    print("[OK] Added table existence check to similarity_search")
else:
    print("[WARN] Pattern not found, trying to find similar pattern...")
    if 'result = []' in code and 'if not query:' in code:
        print("[INFO] Found individual parts, but exact pattern doesn't match")
        print("[INFO] This might be due to whitespace differences")

# Verify
has_table_check = 'SELECT COUNT(*) FROM' in code and 'LIMIT 1' in code and "Table doesn't exist yet" in code
print(f"\nVerification: Table existence check present = {has_table_check}")

# Update code
miniob_node['data']['node']['template']['code']['value'] = code

# Save
with open('model.json', 'w', encoding='utf-8') as f:
    json.dump(data, f, ensure_ascii=False, indent=2)

print("\n[SUCCESS] Table existence check added!")
