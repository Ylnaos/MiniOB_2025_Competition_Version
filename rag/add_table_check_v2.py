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

print("\n=== Adding table existence check to similarity_search ===")

# Find the similarity_search method and add check after "result = []"
# Use regex to handle any whitespace variations
pattern = r'(def similarity_search\([^)]+\)[^:]*:\s+self\.__log_func\([^)]+\)\s+result = \[\]\s*\n)'

# The text to insert
insert_text = '''
        # Check if table exists before querying
        try:
            test_sql = f"SELECT COUNT(*) FROM {self.__table} LIMIT 1"
            self.connector.exec(test_sql)
        except Exception:
            # Table doesn't exist yet, return empty results
            return result
'''

# Try to find and replace
if re.search(pattern, code):
    # Insert the check after "result = []"
    code = re.sub(
        pattern,
        r'\1' + insert_text + '\n',
        code,
        count=1
    )
    print("[OK] Added table existence check to similarity_search")
else:
    print("[WARN] Regex pattern not found")
    # Try simpler approach - find "result = []" in similarity_search context
    # and add code after it
    lines = code.split('\n')
    new_lines = []
    in_similarity_search = False
    added = False

    for i, line in enumerate(lines):
        new_lines.append(line)

        if 'def similarity_search(' in line:
            in_similarity_search = True

        if in_similarity_search and not added and 'result = []' in line:
            # Add the check after this line
            indent = len(line) - len(line.lstrip())
            check_lines = [
                '',
                ' ' * indent + '# Check if table exists before querying',
                ' ' * indent + 'try:',
                ' ' * indent + '    test_sql = f"SELECT COUNT(*) FROM {self.__table} LIMIT 1"',
                ' ' * indent + '    self.connector.exec(test_sql)',
                ' ' * indent + 'except Exception:',
                ' ' * indent + '    # Table doesn\'t exist yet, return empty results',
                ' ' * indent + '    return result',
            ]
            new_lines.extend(check_lines)
            added = True
            in_similarity_search = False
            print("[OK] Added table existence check using line-by-line approach")

    code = '\n'.join(new_lines)

# Verify
has_table_check = 'SELECT COUNT(*) FROM' in code and 'LIMIT 1' in code and "doesn't exist yet" in code
print(f"\nVerification: Table existence check present = {has_table_check}")

# Validate Python syntax
try:
    compile(code, '<string>', 'exec')
    print("[OK] Python syntax is valid")
except SyntaxError as e:
    print(f"[ERROR] Syntax error: {e}")
    exit(1)

# Update code
miniob_node['data']['node']['template']['code']['value'] = code

# Save
with open('model.json', 'w', encoding='utf-8') as f:
    json.dump(data, f, ensure_ascii=False, indent=2)

print("\n[SUCCESS] Table existence check added and verified!")
