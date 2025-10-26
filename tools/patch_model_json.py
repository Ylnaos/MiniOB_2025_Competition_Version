#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
将实现完 TODO 的 MiniOB 组件代码写回 rag/model.json，并补充工作流连线。
仅修改：
- MiniOB 节点的 template.code.value
- data.edges 连接列表
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODEL = ROOT/"rag"/"model.json"
IMPL  = ROOT/"rag"/"_miniob_component.py"

def load_json(p: Path):
    with p.open('r', encoding='utf-8') as f:
        return json.load(f)

def dump_json(p: Path, obj):
    # 保持 UTF-8 与紧凑格式，避免过度重排
    with p.open('w', encoding='utf-8') as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)

def main():
    j = load_json(MODEL)
    code = IMPL.read_text(encoding='utf-8')

    # 1) 更新 MiniOB 组件代码
    for n in j["data"]["nodes"]:
        if n.get("id") == "MiniOB-lrJi1":
            n["data"]["node"]["template"]["code"]["value"] = code
            break
    else:
        raise SystemExit("MiniOB-lrJi1 node not found in model.json")

    # 2) 构造连线（若已存在则覆盖为我们定义的）
    # 构造 ports 元数据
    nodes = {n["id"]: n for n in j["data"]["nodes"]}

    def get_output_types(node_id: str, port_name: str):
        outs = nodes[node_id]["data"]["node"].get("outputs", [])
        for o in outs:
            if o.get("name") == port_name:
                return o.get("types", [])
        return []

    def get_input_meta(node_id: str, field_name: str):
        tmpl = nodes[node_id]["data"]["node"].get("template", {})
        fld = tmpl.get(field_name)
        if not isinstance(fld, dict):
            return {"inputTypes": [], "type": "other"}
        in_types = fld.get("input_types") or fld.get("inputTypes") or []
        ftype = fld.get("type", "other")
        return {"inputTypes": in_types, "type": ftype}

    def dtype(node_id: str):
        node = nodes[node_id]["data"]["node"]
        # 优先用 key；否则用 display_name；再退回 name
        return node.get("key") or node.get("display_name") or node.get("name")

    def to_handle_string(obj: dict) -> str:
        import json as _json
        # 将 JSON 中的双引号替换为 œ 以匹配官方导出形式
        return _json.dumps(obj, ensure_ascii=False).replace('"', 'œ')

    def build_edge(src, src_port, tgt, tgt_field):
        sh_obj = {
            "dataType": dtype(src),
            "id": src,
            "name": src_port,
            "output_types": get_output_types(src, src_port),
        }
        th_meta = get_input_meta(tgt, tgt_field)
        th_obj = {
            "fieldName": tgt_field,
            "id": tgt,
            "inputTypes": th_meta.get("inputTypes", []),
            "type": th_meta.get("type", "other"),
        }
        # 兼容新版：sourceHandle/targetHandle 使用简短端口名
        # 同时 id 仍按兼容旧版的 reactflow__edge 格式生成
        sh_str = to_handle_string(sh_obj)
        th_str = to_handle_string(th_obj)
        edge_id = f"reactflow__edge-{src}{sh_str}-{tgt}{th_str}"
        return {
            "id": edge_id,
            "source": src,
            "target": tgt,
            "sourceHandle": src_port,
            "targetHandle": tgt_field,
            "type": "buttonedge",
            "className": "",
            "animated": False,
            "selected": False,
            "data": {
                "sourceHandle": sh_obj,
                "targetHandle": th_obj,
            },
        }

    edges = []

    # Directory -> Split Text
    edges.append(build_edge("Directory-RSP7d", "dataframe", "SplitText-Tu2iV", "data_inputs"))
    # Split Text -> MiniOB (ingest)
    edges.append(build_edge("SplitText-Tu2iV", "dataframe", "MiniOB-lrJi1", "ingest_data"))
    # Ollama Embeddings -> MiniOB (embedding model)
    edges.append(build_edge("OllamaEmbeddings-vt2mP", "embeddings", "MiniOB-lrJi1", "embedding_model"))
    # Chat Input -> MiniOB (query)
    edges.append(build_edge("ChatInput-VGZSq", "message", "MiniOB-lrJi1", "search_query"))
    # MiniOB -> Parser
    edges.append(build_edge("MiniOB-lrJi1", "search_results", "parser-tiD4c", "input_data"))
    # Parser -> Prompt (context)
    edges.append(build_edge("parser-tiD4c", "parsed_text", "Prompt-cjybM", "context"))
    # Chat Input -> Prompt (question)
    edges.append(build_edge("ChatInput-VGZSq", "message", "Prompt-cjybM", "question"))
    # Prompt -> Qwen
    edges.append(build_edge("Prompt-cjybM", "prompt", "ChatQwenModel-5mSki", "input_value"))
    # Qwen -> Chat Output
    edges.append(build_edge("ChatQwenModel-5mSki", "text_output", "ChatOutput-k7ET8", "input_value"))
    # Qwen -> Test Output
    edges.append(build_edge("ChatQwenModel-5mSki", "text_output", "TestOutput-gbo7V", "input_value"))

    j["data"]["edges"] = edges

    dump_json(MODEL, j)
    print("model.json patched: MiniOB code updated and edges connected (", len(edges), ")")

if __name__ == '__main__':
    main()
