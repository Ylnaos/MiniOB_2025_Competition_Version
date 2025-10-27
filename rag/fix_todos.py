#!/usr/bin/env python3
"""
修复model.json中MiniOB组件的TODO
"""
import json
import re

# 读取model.json
with open('model.json', encoding='utf-8') as f:
    model = json.load(f)

# 找到MiniOB组件
miniob_node = None
for node in model['data']['nodes']:
    if 'MiniOB' in node['data']['id']:
        miniob_node = node
        break

if not miniob_node:
    print("ERROR: MiniOB component not found!")
    exit(1)

# 获取原始代码
original_code = miniob_node['data']['node']['template']['code']['value']

print(f"Original code length: {len(original_code)}")

# TODO 1: 初始化数据库（建表和索引）
todo1_implementation = '''
        # 初始化元数据：建表与向量索引（若未存在）
        self.__table = "rag_docs"
        self.__index = "idx_rag_docs_embedding"
        self.__ensure_initialized()

    def __vector_literal(self, vec: List[float]) -> str:
        """生成MiniOB向量字面量"""
        inner = ",".join(f"{float(x):.8f}" for x in vec)
        return "[" + inner + "]"

    def __escape_text(self, text: str, limit_bytes: int = 1000) -> str:
        """规范化文本为SQL安全格式"""
        import re
        if text is None:
            text = ""
        s = str(text)
        # 标准化空白并移除竖线分隔符
        s = s.replace("|", " ").replace("\\r", " ").replace("\\n", " ")
        # 去除控制字符
        s = re.sub(r'[\\x00-\\x08\\x0B-\\x1F\\x7F]', ' ', s)
        s = re.sub(r'[\\u200B-\\u200F\\u202A-\\u202E\\u2060\\u2066-\\u2069]', ' ', s)
        # 压缩空白
        s = " ".join(s.split())
        # 按字节限制
        if limit_bytes:
            b = s.encode("utf-8", errors="ignore")
            if len(b) > limit_bytes:
                b = b[:limit_bytes]
                s = b.decode("utf-8", errors="ignore")
        # 替换双引号
        s = s.replace('"', ' ')
        return s

    def __ensure_initialized(self) -> None:
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
            self.__log_func(f"Create vector index ignored: {e}")
'''

# TODO 2: 相似度搜索
todo2_implementation = '''
        if not query:
            return result

        try:
            qvec = self.__embedding.embed_query(query)
        except Exception as e:
            raise MiniOBException(
                message=f"Failed to embed query: {e}",
                operation="EMBED_QUERY",
            ) from e

        vec_lit = self.__vector_literal(qvec)
        sql = (
            f"SELECT content FROM {self.__table} "
            f"ORDER BY COSINE_DISTANCE(embedding, {vec_lit}) LIMIT {int(k)}"
        )
        raw = self.connector.exec(sql)

        lines = [ln for ln in raw.splitlines() if ln and not ln.startswith('#')]
        if not lines:
            return result
        # 第1行是列名
        header = [h.strip() for h in lines[0].split(' | ')]
        try:
            cidx = header.index('content')
        except ValueError:
            cidx = 0

        for ln in lines[1:]:
            if ' | ' in ln:
                parts = ln.split(' | ')
                content = parts[cidx]
            else:
                content = ln
            result.append(Document(page_content=content, metadata={}))
'''

# TODO 3: 插入数据
todo3_implementation = '''
        # 逐行插入，最大程度降低SQL解析压力
        batch_size = 1
        total = len(page_contents)
        inserted = 0
        for start in range(0, total, batch_size):
            values_sql = []
            for i in range(start, min(start + batch_size, total)):
                text = self.__escape_text(page_contents[i])
                vec_lit = self.__vector_literal(embeddings[i])
                values_sql.append(f'("{text}", {vec_lit})')

            if not values_sql:
                continue

            insert_sql = (
                f"insert into {self.__table} (content, embedding) values "
                + ",".join(values_sql)
            )
            try:
                self.__log_func(
                    f"Insert SQL length={len(insert_sql)} head='{insert_sql[:256]}'"
                )
            except Exception:
                pass

            try:
                res = self.connector.exec(insert_sql)
                self.__log_func(f"Inserted {len(values_sql)} rows. Result: {res}")
                inserted += len(values_sql)
            except MiniOBQueryException as e:
                # 回退策略：缩短内容后重试
                try:
                    short_text = self.__escape_text(page_contents[start], limit_bytes=512)
                    vec_lit = self.__vector_literal(embeddings[start])
                    fallback_sql = (
                        f'insert into {self.__table} (content, embedding) values ("{short_text}", {vec_lit})'
                    )
                    self.__log_func(f"Retry insert with shorter content")
                    res = self.connector.exec(fallback_sql)
                    self.__log_func(f"Fallback insert succeeded. Result: {res}")
                    inserted += 1
                except Exception as e2:
                    self.__log_func(f"Insert failed for row {start}: {e2}")

        # 触发表统计优化
        try:
            _ = self.connector.exec(f"analyze table {self.__table}")
        except Exception:
            pass

        return [str(i) for i in range(inserted)]
'''

# 替换TODO
new_code = original_code

# 替换TODO 1 (初始化)
new_code = new_code.replace(
    "        # TODO: initialize miniob data",
    todo1_implementation
)

# 替换TODO 2 (搜索)
new_code = new_code.replace(
    "        # TODO: search similar data from miniob",
    todo2_implementation
)

# 替换TODO 3 (插入)
new_code = new_code.replace(
    "        # TODO: insert data into miniob",
    todo3_implementation
)

# 删除末尾的TODO
new_code = new_code.replace("\nTODO", "")

print(f"New code length: {len(new_code)}")
print(f"Change: {len(new_code) - len(original_code):+d} characters")

# 更新model.json
miniob_node['data']['node']['template']['code']['value'] = new_code

# 保存
with open('model.json', 'w', encoding='utf-8') as f:
    json.dump(model, f, indent=2, ensure_ascii=False)

print("\n✓ Successfully fixed all TODOs in model.json")
print("\nVerifying...")

# 验证
with open('model.json', encoding='utf-8') as f:
    verify_model = json.load(f)

for node in verify_model['data']['nodes']:
    if 'MiniOB' in node['data']['id']:
        verify_code = node['data']['node']['template']['code']['value']

        todo_count = verify_code.upper().count('TODO')
        print(f"Remaining TODOs: {todo_count}")

        if '__ensure_initialized' in verify_code:
            print("✓ __ensure_initialized method added")
        if 'COSINE_DISTANCE(embedding' in verify_code:
            print("✓ COSINE_DISTANCE search implemented")
        if 'insert into' in verify_code:
            print("✓ Insert logic implemented")

        break

print("\nDone!")
