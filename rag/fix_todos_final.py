#!/usr/bin/env python3
"""
修复model.json中的3个TODO实现
基于MiniOB源码yacc_sql.y确定的正确SQL语法
"""

import json
import sys

# TODO 1: __init__ 初始化
TODO1_IMPL = '''
        # Initialize MiniOB database table and vector index
        try:
            # Step 1: Drop existing table (ignore errors if table doesn't exist)
            try:
                drop_sql = "DROP TABLE vector_docs"
                self.__log_func(f"Dropping existing table: {drop_sql}")
                self.connector.exec(drop_sql)
                self.__log_func("Table dropped successfully")
            except Exception as e:
                # Table doesn't exist is normal, ignore this error
                self.__log_func(f"Drop table warning (ignored): {str(e)}")

            # Step 2: Create table (Note: MiniOB does NOT support IF NOT EXISTS)
            create_table_sql = f"CREATE TABLE vector_docs (id INT, content TEXT, embedding VECTOR({self.__embedding_dimension}))"
            self.__log_func(f"Creating table with SQL: {create_table_sql}")
            self.connector.exec(create_table_sql)
            self.__log_func("Table created successfully")

            # Step 3: Create vector index (Note: WITH has space, parameters in parentheses)
            create_index_sql = f"CREATE VECTOR INDEX vec_idx ON vector_docs (embedding) WITH (TYPE=IVFFLAT, DISTANCE=L2_DISTANCE, LISTS=245, PROBES=5)"
            self.__log_func(f"Creating vector index with SQL: {create_index_sql}")
            try:
                self.connector.exec(create_index_sql)
                self.__log_func("Vector index created successfully")
            except Exception as e:
                # Index may already exist, log but don't raise
                self.__log_func(f"Index creation warning (may already exist): {str(e)}")

        except Exception as e:
            self.__log_func(f"Error during table/index initialization: {str(e)}")
            raise MiniOBException(
                message=f"Failed to initialize MiniOB tables: {str(e)}",
                operation="TABLE_INITIALIZATION",
                details={"embedding_dimension": self.__embedding_dimension},
            ) from e
'''

# TODO 2: similarity_search 实现
TODO2_IMPL = '''
        try:
            # Generate query embedding
            self.__log_func("Generating embedding for query...")
            query_embedding = self.__embedding.embed_query(query)
            self.__log_func(f"Query embedding generated with dimension {len(query_embedding)}")

            # Convert to string format '[1,2,3]' (Note: needs single quotes)
            embedding_str = '[' + ','.join([str(x) for x in query_embedding]) + ']'

            # Execute vector search (Note: vector literal with single quotes)
            search_sql = f"SELECT id, content FROM vector_docs ORDER BY L2_DISTANCE(embedding, '{embedding_str}') LIMIT {k}"
            self.__log_func(f"Executing search SQL: {search_sql[:200]}...")

            result_str = self.connector.exec(search_sql)
            self.__log_func(f"Search result: {result_str[:500]}...")

            # Parse result (Format: each line "id | content")
            if result_str and result_str.strip():
                lines = result_str.strip().split('\\n')
                for line in lines:
                    # Skip header and separator lines
                    if not line.strip() or '|' not in line or line.startswith('ID') or line.startswith('id') or line.startswith('---'):
                        continue

                    try:
                        parts = line.split('|')
                        if len(parts) >= 2:
                            content = parts[1].strip()
                            if content:
                                doc = Document(page_content=content, metadata={})
                                result.append(doc)
                                self.__log_func(f"Added document: {content[:100]}...")
                    except Exception as parse_err:
                        self.__log_func(f"Error parsing line '{line}': {str(parse_err)}")
                        continue
            else:
                self.__log_func("No results returned from vector search")

        except Exception as e:
            self.__log_func(f"Error during vector search: {str(e)}")
            raise MiniOBException(
                message=f"Vector search failed: {str(e)}",
                operation="VECTOR_SEARCH",
                details={"query": query[:100], "k": k}
            ) from e
'''

# TODO 3: add_documents 实现
TODO3_IMPL = '''
        # Clear old data to avoid ID conflicts
        try:
            self.__log_func("Clearing old data from vector_docs...")
            self.connector.exec("DELETE FROM vector_docs")
            self.__log_func("Old data cleared successfully")
        except Exception as e:
            self.__log_func(f"Warning: Failed to clear old data: {str(e)}")

        # Insert new documents
        inserted_ids = []
        self.__log_func(f"Inserting {len(documents)} documents into MiniOB...")

        for idx, (doc, emb) in enumerate(zip(documents, embeddings)):
            try:
                # Escape single quotes to prevent SQL injection
                content = doc.page_content.replace("'", "''")
                # Truncate if too long
                if len(content) > 10000:
                    content = content[:10000]
                    self.__log_func(f"Content truncated for document {idx}")

                # Convert embedding to vector string (Note: wrap with single quotes)
                emb_str = '[' + ','.join([str(x) for x in emb]) + ']'

                # Insert SQL (Note: vector with single quotes)
                insert_sql = f"INSERT INTO vector_docs (id, content, embedding) VALUES ({idx}, '{content}', '{emb_str}')"
                self.__log_func(f"Inserting document {idx}/{len(documents)}...")

                result = self.connector.exec(insert_sql)
                self.__log_func(f"Insert result for doc {idx}: {result}")
                inserted_ids.append(str(idx))

            except Exception as e:
                self.__log_func(f"Error inserting document {idx}: {str(e)}")
                # Continue with next document
                continue

        self.__log_func(f"Successfully inserted {len(inserted_ids)}/{len(documents)} documents")
        return inserted_ids
'''


def main():
    model_json_path = "D:\\迅雷下载\\MiniOB2025\\MiniOB_2025_Competition_Version\\rag\\model.json"

    print(f"Reading {model_json_path}...")
    with open(model_json_path, 'r', encoding='utf-8') as f:
        data = json.load(f)

    # Find MiniOB node
    found = False
    for node in data.get('data', {}).get('nodes', []):
        if 'MiniOB' in node.get('data', {}).get('type', ''):
            print("Found MiniOB node, modifying code...")
            code = node['data']['node']['template']['code']['value']

            # Replace TODO 1
            if '# TODO: initialize miniob data' in code:
                code = code.replace('# TODO: initialize miniob data', TODO1_IMPL.strip())
                print("[OK] Replaced TODO 1: initialize miniob data")

            # Replace TODO 2
            if '# TODO: search similar data from miniob' in code:
                code = code.replace('# TODO: search similar data from miniob', TODO2_IMPL.strip())
                print("[OK] Replaced TODO 2: search similar data from miniob")

            # Replace TODO 3
            if '# TODO: insert data into miniob' in code:
                code = code.replace('# TODO: insert data into miniob', TODO3_IMPL.strip())
                print("[OK] Replaced TODO 3: insert data into miniob")

            # Update code
            node['data']['node']['template']['code']['value'] = code
            found = True
            break

    if not found:
        print("ERROR: MiniOB node not found!")
        sys.exit(1)

    # Write back
    print(f"Writing back to {model_json_path}...")
    with open(model_json_path, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)

    print("[OK] All TODOs replaced successfully!")
    print("\nNext steps:")
    print("1. Test the workflow in Langflow")
    print("2. Check MiniOB logs for any SQL errors")
    print("3. Verify recall rate improves from 50 to 60+")

if __name__ == '__main__':
    main()
