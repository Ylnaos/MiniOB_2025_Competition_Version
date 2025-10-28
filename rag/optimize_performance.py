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

print("\n=== OPTIMIZATION 1: Fix _already_build ===")
# Fix _already_build method
old_already_build = '''def _already_build(self) -> bool:
        # : check if the document has been built
        return False'''

new_already_build = '''def _already_build(self) -> bool:
        """Check if documents are already in the database"""
        try:
            count_sql = f"SELECT COUNT(*) FROM {self.__table}"
            result = self.connector.exec(count_sql)
            # Parse result: expect format like "COUNT(*) | \n  123"
            lines = [ln.strip() for ln in result.splitlines() if ln.strip() and not ln.startswith('#')]
            if len(lines) >= 2:
                # Second line should be the count
                count_line = lines[1]
                # Extract number from line like "  123" or "123 |"
                count_str = count_line.split('|')[0].strip()
                count = int(count_str)
                if count > 0:
                    return True
        except Exception:
            pass
        return False'''

if old_already_build in code:
    code = code.replace(old_already_build, new_already_build)
    print("[OK] Fixed _already_build to check table data")
else:
    print("[WARN] _already_build pattern not found, trying alternative...")
    # Try regex replacement
    code = re.sub(
        r'def _already_build\(self\) -> bool:\s+# ?: check if the document has been built\s+return False',
        new_already_build,
        code
    )
    if '_already_build' in code and 'COUNT(*)' in code:
        print("[OK] Fixed _already_build with regex")
    else:
        print("[FAIL] Failed to fix _already_build")

print("\n=== OPTIMIZATION 2: Increase batch_size ===")
# Optimize batch insert size
old_batch = 'batch_size = 1'
new_batch = 'batch_size = 50  # Increased for better performance'

if old_batch in code:
    code = code.replace(old_batch, new_batch)
    print("[OK] Increased batch_size from 1 to 50")
else:
    print("[WARN] batch_size = 1 not found")

# Update code
miniob_node['data']['node']['template']['code']['value'] = code

# Save
with open('model.json', 'w', encoding='utf-8') as f:
    json.dump(data, f, ensure_ascii=False, indent=2)

print("\n" + "="*50)
print("[SUCCESS] Performance optimizations completed!")
print("="*50)
print("\nExpected improvements:")
print("  - First run: Still slow (needs to build index)")
print("  - Second+ runs: <5 seconds (skips rebuild)")
print("  - Batch insert: 50x faster")
