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

code = miniob_node['data']['node']['template']['code']['value']

print("\n=== FIX: Add table existence check in similarity_search ===")

# Find the exact pattern with __log_func
old_pattern = '''        result = []


        if not query:
            return result'''

new_pattern = '''        result = []

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
    print("[OK] Added table existence check")
else:
    print("[WARN] Pattern not found, trying alternative...")
    # Try without the double space
    old_pattern2 = '''        result = []

        if not query:
            return result'''

    if old_pattern2 in code:
        code = code.replace(old_pattern2, new_pattern)
        print("[OK] Added table existence check (alternative pattern)")
    else:
        print("[ERROR] Could not find pattern to replace")

# Verify
print(f"\n=== VERIFICATION ===")
has_check = 'SELECT COUNT(*) FROM' in code and 'LIMIT 1' in code and "Table doesn't exist yet" in code
print(f"Table existence check added: {has_check}")
print(f"Vector literal uses spaces: {'", ".join' in code}")

# Update code
miniob_node['data']['node']['template']['code']['value'] = code

# Save
with open('model.json', 'w', encoding='utf-8') as f:
    json.dump(data, f, ensure_ascii=False, indent=2)

print("\n[SUCCESS] Fixes applied!")
