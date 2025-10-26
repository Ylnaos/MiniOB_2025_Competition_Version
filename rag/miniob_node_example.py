# -*- coding: utf-8 -*-
"""
可直接粘贴到 Langflow 的“Python/Custom Code”节点（即题目中的 MiniOB 组件）

输入（建议）：
- question: str  问题文本
- top_k: int     检索条数（默认 5）

可选输入（用于首次写库）：
- docs: List[Dict]  形如 [{"path":..., "title":..., "content":..., "embedding":[...]}]

输出：
{
  "candidates": [
     {"path": str, "title": str, "content": str, "score": float}
  ],
  "raw_sql": str
}

环境变量：
- MINIOB_SERVER_SOCKET  MiniOB 的 Unix Socket 路径
"""
import os
import json
from typing import List, Dict, Any

try:
    import pymysql
except Exception:
    pymysql = None


SUPPORT_FACTS = {
    # 规则优先使用“问题文本”触发，命中即带入召回标签
    "标识符长度限制": [
        "集群名", "租户名", "用户名", "最大长度", "标识符长度",
    ],
    "ODP(OceanBase Database Proxy) 连接限制": [
        "ODP", "OceanBase Database Proxy", "连接限制",
    ],
    "单个表的限制": [
        "单个表", "行长度", "列数", "索引个数", "最大限制",
    ],
    "字符串类型限制": [
        "VARCHAR", "字符", "字符串", "最大长度",
    ],
    "功能使用限制": [
        "物理备库", "功能使用限制", "限制",
    ],
}


def classify_support_facts(question: str, contexts: List[str]) -> List[str]:
    q = (question or "").lower()
    found = []
    for label, keys in SUPPORT_FACTS.items():
        for k in keys:
            if k.lower() in q:
                found.append(label)
                break
    # 如果问题触发不到，则尝试从上下文标题/正文中粗略匹配
    if not found:
        joined = "\n".join(contexts).lower()
        for label, keys in SUPPORT_FACTS.items():
            for k in keys:
                if k.lower() in joined:
                    found.append(label)
                    break
    # 去重，稳定顺序
    seen = set()
    result = []
    for x in found:
        if x not in seen:
            seen.add(x)
            result.append(x)
    return result


def _connect():
    sock = os.getenv("MINIOB_SERVER_SOCKET", "")
    if not sock:
        raise RuntimeError("缺少环境变量 MINIOB_SERVER_SOCKET")
    if pymysql is None:
        raise RuntimeError("未安装 pymysql，无法连接 MiniOB")
    return pymysql.connect(host="localhost", user="root", password="", unix_socket=sock, autocommit=True)


def _ensure_schema(conn):
    with conn.cursor() as cur:
        cur.execute("CREATE TABLE IF NOT EXISTS ob_docs (id INT, path STRING(512) NULL, title STRING(256) NULL, content TEXT NULL, embedding VECTOR(1024) NULL)")
        cur.execute("CREATE VECTOR INDEX IF NOT EXISTS idx_ob_docs_vec ON ob_docs { embedding } WITH { TYPE = IVFFLAT, DISTANCE = COSINE_DISTANCE, LISTS = 100, PROBES = 10 }")


def _bulk_insert(conn, docs: List[Dict[str, Any]]):
    if not docs:
        return
    with conn.cursor() as cur:
        rid = 1
        for d in docs:
            path = (d.get("path") or "")[:512]
            title = (d.get("title") or "")[:256]
            content = d.get("content") or ""
            vec = d.get("embedding")
            if not isinstance(vec, list):
                continue
            vec_literal = json.dumps(vec, ensure_ascii=False)
            cur.execute(
                "INSERT INTO ob_docs(id, path, title, content, embedding) VALUES(%s, %s, %s, %s, TO_VECTOR(%s))",
                (rid, path, title, content, vec_literal),
            )
            rid += 1


def _search(conn, query_vec: List[float], top_k: int = 5):
    # 余弦距离越小越相似
    vec_literal = json.dumps(query_vec, ensure_ascii=False)
    sql = (
        "SELECT path, title, content, DISTANCE(embedding, TO_VECTOR(%s), 'COSINE') AS score "
        "FROM ob_docs ORDER BY score ASC LIMIT %s"
    )
    with conn.cursor() as cur:
        cur.execute(sql, (vec_literal, int(top_k)))
        rows = cur.fetchall()
    # PyMySQL 默认返回 tuple
    results = []
    for r in rows:
        results.append({
            "path": r[0],
            "title": r[1],
            "content": r[2],
            "score": float(r[3]) if r[3] is not None else 1e9,
        })
    return results, sql


def run(question: str, query_embedding: List[float], top_k: int = 5, docs: List[Dict[str, Any]] = None) -> Dict[str, Any]:
    conn = _connect()
    _ensure_schema(conn)
    if docs:
        _bulk_insert(conn, docs)
    candidates, raw_sql = _search(conn, query_embedding, top_k)
    contexts = [c.get("title") or "" for c in candidates] + [c.get("content") or "" for c in candidates]
    support_facts = classify_support_facts(question or "", contexts)
    return {
        "candidates": candidates,
        "support_facts": support_facts,
        "raw_sql": raw_sql,
    }

