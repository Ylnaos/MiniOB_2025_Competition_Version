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

print("\n=== CRITICAL FIX: Remove broken _already_build method ===")

# The _already_build method references self.__table and self.connector
# which don't exist in Component class - they only exist in VectorStore class
# This causes AttributeError crash

# Find and remove the entire _already_build method
old_method = '''    def _already_build(self) -> bool:
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
    code = code.replace(old_method, '')
    print("[OK] Removed broken _already_build method")
else:
    print("[WARN] Exact pattern not found, trying with different indentation...")
    # Try to find any _already_build method and remove it
    lines = code.split('\n')
    new_lines = []
    in_already_build = False
    skip_count = 0

    for line in lines:
        if 'def _already_build(self)' in line:
            in_already_build = True
            skip_count = 0
            print("[OK] Found _already_build method, removing...")
            continue

        if in_already_build:
            # Skip lines until we find the next method definition or class end
            if line.strip() and not line.startswith('        ') and not line.startswith('\t\t'):
                # Found next method or end of indentation
                in_already_build = False
                new_lines.append(line)
            elif 'def ' in line and line.strip().startswith('def '):
                # Found next method
                in_already_build = False
                new_lines.append(line)
            else:
                skip_count += 1
        else:
            new_lines.append(line)

    code = '\n'.join(new_lines)
    print(f"[OK] Removed _already_build method ({skip_count} lines)")

print("\n=== FIX 2: Modify build_vector_store to check before adding documents ===")

# Replace the build logic to check table directly
old_build = '''        self.log("Vector Store created successfully")
        if not self._already_build():
            self._add_documents_to_vector_store(vector_store)
        self.log("Vector Store build completed")
        return vector_store'''

new_build = '''        self.log("Vector Store created successfully")

        # Check if table already has data to avoid rebuilding
        try:
            count_sql = "SELECT COUNT(*) FROM rag_docs"
            result = vector_store.connector.exec(count_sql)
            lines = [ln.strip() for ln in result.splitlines() if ln.strip() and not ln.startswith('#')]
            has_data = False
            if len(lines) >= 2:
                count_line = lines[1]
                count_str = count_line.split('|')[0].strip()
                count = int(count_str)
                has_data = count > 0

            if not has_data:
                self._add_documents_to_vector_store(vector_store)
            else:
                self.log(f"Table already has {count} documents, skipping rebuild")
        except Exception as e:
            # Table doesn't exist or error checking, proceed with normal build
            self.log(f"Could not check existing data: {e}, proceeding with document ingestion")
            self._add_documents_to_vector_store(vector_store)

        self.log("Vector Store build completed")
        return vector_store'''

if old_build in code:
    code = code.replace(old_build, new_build)
    print("[OK] Updated build_vector_store to check table directly")
else:
    print("[WARN] build_vector_store pattern not found")

# Validate Python syntax
try:
    compile(code, '<string>', 'exec')
    print("\n[OK] Python syntax is valid")
except SyntaxError as e:
    print(f"\n[ERROR] Syntax error: {e}")
    exit(1)

# Update code
miniob_node['data']['node']['template']['code']['value'] = code

# Save
with open('model.json', 'w', encoding='utf-8') as f:
    json.dump(data, f, ensure_ascii=False, indent=2)

print("\n[SUCCESS] Critical fix applied!")
print("\nWhat changed:")
print("  1. Removed broken _already_build() method that caused AttributeError")
print("  2. Moved table check into build_vector_store() where vector_store.connector is available")
print("  3. Now safely checks table data before rebuilding")
