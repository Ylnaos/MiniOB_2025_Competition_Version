#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
miniob_loader.py

用途：离线一次性构建向量库（把 OceanBase 文档分块并写入 MiniOB）。

前置：
- 已启动 MiniOB，并能通过 UNIX Socket 连接（MINIOB_SERVER_SOCKET）
- 已有嵌入服务（EMBEDDING_BASE_URL）与模型名（EMBEDDING_NAME，例如 bge-m3）
- OB_DOC_PATH 指向本地 OceanBase 文档根目录（推荐 zh-CN 子目录）

注意：评测时 Langflow 会在线串起 Directory/Split/Embeddings/MiniOB；本脚本是“预热/备份”方案，
可以先把索引构建好，确保在线流程稳定。如果你的 Langflow 自定义节点已实现写库逻辑，本脚本可不跑。
"""
import os
import sys
import json
import time
import glob
import math
import socket
from typing import List, Dict, Tuple

try:
    import pymysql  # 纯 Python MySQL 协议实现，支持 unix_socket
except Exception as e:
    pymysql = None

try:
    import requests
except Exception:
    requests = None


def read_env(name: str, required: bool = True, default: str = "") -> str:
    val = os.getenv(name, default)
    if required and not val:
        print(f"[ERROR] 缺少环境变量 {name}", file=sys.stderr)
        sys.exit(2)
    return val


def iter_text_files(root: str) -> List[str]:
    exts = ["**/*.md", "**/*.txt", "**/*.rst", "**/*.html"]
    files = []
    for p in exts:
        files.extend(glob.glob(os.path.join(root, p), recursive=True))
    return files


def simple_read(path: str) -> str:
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            return f.read()
    except Exception:
        try:
            with open(path, "r", encoding="gb18030", errors="ignore") as f:
                return f.read()
        except Exception:
            return ""


def split_text(text: str, chunk_size: int = 500, overlap: int = 64) -> List[str]:
    # 简单按字符滑窗切分，避免额外依赖
    text = text.strip()
    chunks = []
    i = 0
    n = len(text)
    while i < n:
        j = min(i + chunk_size, n)
        chunk = text[i:j]
        if chunk.strip():
            chunks.append(chunk)
        if j == n:
            break
        i = j - overlap
        if i < 0:
            i = 0
    return chunks


def embed_batch(base_url: str, model: str, inputs: List[str]) -> List[List[float]]:
    if requests is None:
        raise RuntimeError("requests 未安装，无法调用嵌入服务")
    url = base_url.rstrip("/") + "/api/embeddings"
    # 兼容常见 /embeddings 或 /v1/embeddings 风格
    alt_url = base_url.rstrip("/") + "/v1/embeddings"
    payload = {"model": model, "input": inputs}

    for u in (url, alt_url):
        try:
            r = requests.post(u, json=payload, timeout=60)
            r.raise_for_status()
            data = r.json()
            # 兼容不同返回结构
            if isinstance(data, dict) and "data" in data and isinstance(data["data"], list):
                vectors = [item.get("embedding") for item in data["data"]]
            elif isinstance(data, dict) and "embeddings" in data:
                vectors = data["embeddings"]
            else:
                # Ollama 风格：{"embedding": [...]} for single; 或 {"embeddings": [[...], ...]}
                vectors = data.get("embedding") or data.get("embeddings")
                if isinstance(vectors, list) and vectors and not isinstance(vectors[0], list):
                    vectors = [vectors]
            if not vectors:
                raise RuntimeError(f"嵌入返回为空：{data}")
            return vectors
        except Exception as e:
            last_err = e
    raise RuntimeError(f"调用嵌入失败：{last_err}")


def ensure_schema(conn):
    ddl = open(os.path.join(os.path.dirname(__file__), "miniob_schema.sql"), "r", encoding="utf-8").read()
    with conn.cursor() as cur:
        for stmt in [s.strip() for s in ddl.split(";") if s.strip()]:
            cur.execute(stmt)
    conn.commit()


def insert_rows(conn, rows: List[Tuple[int, str, str, str, List[float]]]):
    # rows: (id, path, title, content, embedding)
    with conn.cursor() as cur:
        for rid, path, title, content, vec in rows:
            # MiniOB 语法：向量字面量可以用字符串+[1,2]或 TO_VECTOR('[1,2]')
            vec_literal = json.dumps(vec, ensure_ascii=False)
            sql = (
                "INSERT INTO ob_docs(id, path, title, content, embedding) "
                "VALUES(%s, %s, %s, %s, TO_VECTOR(%s))"
            )
            cur.execute(sql, (rid, path[:512] if path else None, title[:256] if title else None, content, vec_literal))
    conn.commit()


def connect_miniob(unix_socket: str):
    if pymysql is None:
        raise RuntimeError("未安装 pymysql，无法连接 MiniOB。请在 Langflow/环境中安装或改用已有 MiniOB 节点。")
    return pymysql.connect(host="localhost", user="root", password="", unix_socket=unix_socket, autocommit=True)


def main():
    ob_root = read_env("OB_DOC_PATH")
    model = read_env("EMBEDDING_NAME")
    base  = read_env("EMBEDDING_BASE_URL")
    sock  = read_env("MINIOB_SERVER_SOCKET")

    files = iter_text_files(ob_root)
    if not files:
        print(f"[WARN] 在 {ob_root} 未找到文档文件")
        return 0

    print(f"[INFO] 待处理文件 {len(files)} 个")
    conn = connect_miniob(sock)
    ensure_schema(conn)

    rid = 1
    batch_texts: List[str] = []
    batch_rows: List[Tuple[int,str,str,str]] = []
    batch_size = 8

    for p in files:
        raw = simple_read(p)
        if not raw:
            continue
        chunks = split_text(raw, 500, 64)
        for ch in chunks:
            batch_texts.append(ch)
            batch_rows.append((rid, p, "", ch))
            rid += 1

            if len(batch_texts) >= batch_size:
                vecs = embed_batch(base, model, batch_texts)
                to_insert = [(*batch_rows[i], vecs[i]) for i in range(len(batch_texts))]
                insert_rows(conn, to_insert)
                batch_texts.clear()
                batch_rows.clear()

    if batch_texts:
        vecs = embed_batch(base, model, batch_texts)
        to_insert = [(*batch_rows[i], vecs[i]) for i in range(len(batch_texts))]
        insert_rows(conn, to_insert)

    print("[OK] ob_docs 已写入完成")
    return 0


if __name__ == "__main__":
    sys.exit(main())

