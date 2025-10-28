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

print("\n=== OPTIMIZATION 1: Fix _already_build ===")

# Note: comment is "# :" not "# TODO:"
old_method = '''def _already_build(self) -> bool:
        # : check if the document has been built
        return False'''

new_method = '''def _already_build(self) -> bool:
        """Check if documents are already in the database"""
        try:
            count_sql = f"SELECT COUNT(*) FROM {self.__table}"
            result = self.connector.exec(count_sql)
            lines = [ln.strip() for ln in result.splitlines() if ln.strip() and not ln.startswith('#')]
            if len(lines) >= 2:
                count_line = lines[1]
                count_str = count_line.split('|')[0].strip()
                count = int(count_str)
                if count > 0:
                    return True
        except Exception:
            pass
        return False'''

if old_method in code:
    code = code.replace(old_method, new_method)
    print("[OK] Fixed _already_build method")
else:
    print("[WARN] _already_build pattern not found, trying with spaces...")
    # Try with different indentation
    old_method2 = '''    def _already_build(self) -> bool:
        # : check if the document has been built
        return False'''

    new_method2 = '''    def _already_build(self) -> bool:
        """Check if documents are already in the database"""
        try:
            count_sql = f"SELECT COUNT(*) FROM {self.__table}"
            result = self.connector.exec(count_sql)
            lines = [ln.strip() for ln in result.splitlines() if ln.strip() and not ln.startswith('#')]
            if len(lines) >= 2:
                count_line = lines[1]
                count_str = count_line.split('|')[0].strip()
                count = int(count_str)
                if count > 0:
                    return True
        except Exception:
            pass
        return False'''

    if old_method2 in code:
        code = code.replace(old_method2, new_method2)
        print("[OK] Fixed _already_build method (with indentation)")
    else:
        print("[ERROR] Could not find _already_build pattern")

# Verify
print(f"Contains COUNT(*): {'COUNT(*)' in code}")
print(f"Contains batch_size = 50: {'batch_size = 50' in code}")

# Update code
miniob_node['data']['node']['template']['code']['value'] = code

# Save
with open('model.json', 'w', encoding='utf-8') as f:
    json.dump(data, f, ensure_ascii=False, indent=2)

print("\n[SUCCESS] Optimizations completed!")
