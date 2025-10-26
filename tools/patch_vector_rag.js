#!/usr/bin/env node
// 用法：node tools/patch_vector_rag.js <输入JSON路径> [-o 输出JSON路径]
// 作用：将官方未配置模板改造成符合 MiniOB 评测要求的 Flow

const fs = require('fs');
const path = require('path');

function readJson(p) {
  return JSON.parse(fs.readFileSync(p, 'utf8'));
}
function writeJson(p, obj) {
  fs.writeFileSync(p, JSON.stringify(obj, null, 2), 'utf8');
}

function idOf(obj, fallbackId, finders = []) {
  for (const f of finders) {
    const n = obj.nodes.find(f);
    if (n) return n.id;
  }
  return fallbackId;
}

function ensureNode(obj, id, type, label, data) {
  let n = obj.nodes.find(n => n.id === id);
  if (!n) {
    n = { id, type, label, data: { ...data }, position: [50, 50] };
    obj.nodes.push(n);
  } else {
    n.type = type; n.label = label; n.data = { ...n.data, ...data };
  }
  return n;
}

function ensureEdge(obj, source, target, label) {
  if (!obj.edges) obj.edges = [];
  if (!obj.edges.find(e => e.source === source && e.target === target)) {
    obj.edges.push({ source, target, label });
  }
}

function removeEdge(obj, source, target) {
  obj.edges = (obj.edges || []).filter(e => !(e.source === source && e.target === target));
}

function ensureEnvVars(obj, names) {
  obj.env_vars = Array.isArray(obj.env_vars) ? obj.env_vars : [];
  for (const n of names) if (!obj.env_vars.includes(n)) obj.env_vars.push(n);
}

function miniobScript(vectorDim = 1024) {
  return (
    "import os, json\n"+
    "try:\n    import pymysql\nexcept Exception:\n    pymysql = None\n\n"+
    "LABELS = {\n    '标识符长度限制': ['集群名','租户名','用户名','标识符长度','最大长度'],\n    'ODP(OceanBase Database Proxy) 连接限制': ['ODP','OceanBase Database Proxy','连接限制'],\n    '单个表的限制': ['单个表','行长度','列数','索引个数','最大限制'],\n    '字符串类型限制': ['VARCHAR','字符串','字符','最大长度'],\n    '功能使用限制': ['物理备库','功能使用限制','限制'],\n}\n\n"+
    "def _connect():\n    sock = os.getenv('MINIOB_SERVER_SOCKET','')\n    if not sock: raise RuntimeError('缺少 MINIOB_SERVER_SOCKET')\n    if pymysql is None: raise RuntimeError('未安装 pymysql')\n    return pymysql.connect(host='localhost', user='root', password='', unix_socket=sock, autocommit=True)\n\n"+
    `def _ensure(conn):
    with conn.cursor() as c:
        c.execute("""CREATE TABLE IF NOT EXISTS ob_docs (
  id INT,
  path STRING(512) NULL,
  title STRING(256) NULL,
  content TEXT NULL,
  embedding VECTOR(${vectorDim}) NULL
)""")
        c.execute("""CREATE VECTOR INDEX IF NOT EXISTS idx_ob_docs_vec ON ob_docs { embedding }
  WITH { TYPE = IVFFLAT, DISTANCE = COSINE_DISTANCE, LISTS = 100, PROBES = 10 }""")
`+
    "def _search(conn, qvec, k):\n    vec_lit = json.dumps(qvec, ensure_ascii=False)\n    sql = (\"SELECT path, title, content, DISTANCE(embedding, TO_VECTOR(%s), 'COSINE') AS score FROM ob_docs ORDER BY score ASC LIMIT %s\")\n    with conn.cursor() as c:\n        c.execute(sql, (vec_lit, int(k)))\n        rows = c.fetchall()\n    cands = []\n    for r in rows:\n        cands.append({'path': r[0], 'title': r[1], 'content': r[2], 'score': float(r[3]) if r[3] is not None else 1e9})\n    return cands\n\n"+
    "def _classify(question, contexts):\n    q = (question or '').lower()\n    out = []\n    for lab, keys in LABELS.items():\n        if any(k.lower() in q for k in keys): out.append(lab)\n    if not out:\n        joined = ('\\n'.join(contexts)).lower()\n        for lab, keys in LABELS.items():\n            if any(k.lower() in joined for k in keys): out.append(lab)\n    seen, res = set(), []\n    for x in out:\n        if x not in seen: seen.add(x); res.append(x)\n    return res\n\n"+
    "# params: { 'question': str, 'embedding' 或 'vectors': [float], 'k': int }\n"+
    "def run(question: str = '', embedding: list = None, vectors: list = None, k: int = 5, **kwargs):\n    qvec = embedding or vectors or []\n    if not isinstance(qvec, list) or not qvec:\n        raise RuntimeError('缺少查询向量，请确认 Embeddings 已连接到 MiniOB 节点')\n    conn = _connect()\n    _ensure(conn)\n    cands = _search(conn, qvec, k or 5)\n    contexts = [ (c.get('title') or '') + ' ' + (c.get('content') or '') for c in cands ]\n    facts = _classify(question or '', contexts)\n    return {'candidates': cands, 'support_facts': facts}\n"
  );
}

function parserScript() {
  return (
    "# 解析 MiniOB 结果为上下文与支持事实\n"+
    "import json\n\n"+
    "def run(results: dict, **kwargs):\n    cands = (results or {}).get('candidates') or []\n    support = (results or {}).get('support_facts') or []\n    parts = []\n    for c in cands:\n        t = (c.get('title') or '').strip()\n        x = (c.get('content') or '').strip()\n        if t: parts.append(t)\n        if x: parts.append(x)\n    context = '\\n\\n---\\n\\n'.join(parts[:10])\n    context += '\\n\\nSUPPORT_FACTS_TAGS: ' + json.dumps(support, ensure_ascii=False)\n    return { 'context': context, 'support_facts': support }\n"
  );
}

function main() {
  const args = process.argv.slice(2);
  if (!args[0]) {
    console.error('用法: node tools/patch_vector_rag.js <输入JSON> [-o 输出JSON]');
    process.exit(2);
  }
  const inPath = path.resolve(args[0]);
  const outIdx = args.indexOf('-o');
  const outPath = outIdx > -1 && args[outIdx + 1] ? path.resolve(args[outIdx + 1]) : inPath.replace(/\.json$/i, '_miniob.json');

  const obj = readJson(inPath);
  obj.nodes = Array.isArray(obj.nodes) ? obj.nodes : [];
  obj.edges = Array.isArray(obj.edges) ? obj.edges : [];

  // 1) 环境变量
  ensureEnvVars(obj, [
    'OB_DOC_PATH','EMBEDDING_NAME','EMBEDDING_BASE_URL',
    'LLM_NAME','LLM_API_KEY','LLM_BASE_URL',
    'QA_SERVER_GET_QUESTION_URL','QA_SERVER_POST_ANSWER_URL',
    'MINIOB_SERVER_SOCKET'
  ]);

  // 2) 节点：尽量复用，若不存在就创建
  const dirId  = idOf(obj, 'directory_loader', [n=>n.type==='Directory']);
  const splitId= idOf(obj, 'split_text', [n=>n.type==='SplitText']);
  const embId  = idOf(obj, 'ollama_embeddings', [n=>/embedding/i.test(n.label||'')||/OllamaEmbeddings/.test(n.type||'')]);
  const vsId   = idOf(obj, 'vector_index', [n=>/VectorStore/i.test(n.type||'')]);
  const chatId = idOf(obj, 'chat_input', [n=>n.type==='HTTPRequest' && /GET/i.test((n.data||{}).method||'')]);
  const miniId = idOf(obj, 'miniob_node', [n=>/MiniOB/i.test(n.type||'') || /MiniOB/i.test(n.label||'')]);
  const parserId=idOf(obj,'parser',[n=>/Parser/i.test(n.label||'')]);
  const promptId=idOf(obj,'prompt',[n=>n.type==='Prompt']);
  const llmId  = idOf(obj, 'qwen_llm', [n=>n.type==='LLM']);
  const postId = idOf(obj, 'test_output', [n=>n.type==='HTTPRequest' && /POST/i.test((n.data||{}).method||'')]);

  ensureNode(obj, dirId, 'Directory', 'DirectoryLoader', { path: '${OB_DOC_PATH}', recursive: true, file_extensions: ['.md','.txt','.rst','.html'] });
  ensureNode(obj, splitId, 'SplitText', 'SplitText', { chunk_size: 500, chunk_overlap: 64, split_by: 'characters' });
  ensureNode(obj, embId, 'OllamaEmbeddings', 'OllamaEmbeddings', { model: '${EMBEDDING_NAME}', base_url: '${EMBEDDING_BASE_URL}', batch_size: 16 });
  ensureNode(obj, vsId, 'VectorStore', 'InMemoryVectorStore', { store_type: 'inmemory', onnx_optim: false });

  ensureNode(obj, chatId, 'HTTPRequest', 'Chat Input (GET Question)', { method: 'GET', url: '${QA_SERVER_GET_QUESTION_URL}', headers: {}, timeout: 10 });

  // MiniOB 节点
  ensureNode(obj, miniId, 'MiniOB', 'MiniOB Connector', { k: 5, timeout: 10, script: miniobScript(1024) });

  // Parser
  ensureNode(obj, parserId, 'CustomParser', 'Parser', { script: parserScript() });

  // Prompt/LLM/POST
  ensureNode(obj, promptId, 'Prompt', 'Prompt', {
    template: (
      '你是严谨的数据库文档助手, 仅基于下方上下文回答。\n'+
      '- 引用上下文中的事实, 不臆断。\n'+
      '- 答案最后必须严格原样复制一行: support_facts: 与 SUPPORT_FACTS_TAGS 中的列表一致。\n\n'+
      'Context:\n{context}\n\nQuestion:\n{input}\n\nAnswer:'
    )
  });
  ensureNode(obj, llmId, 'LLM', 'Qwen (LLM)', { model: '${LLM_NAME}', base_url: '${LLM_BASE_URL}', api_key_env: '${LLM_API_KEY}', temperature: 0, max_tokens: 512 });
  ensureNode(obj, postId, 'HTTPRequest', 'Test Output (POST Answer)', { method: 'POST', url: '${QA_SERVER_POST_ANSWER_URL}', headers: { 'Content-Type': 'application/json' }, body_template: '{ "answer": "{output}", "support_facts": {support_facts} }', timeout: 10 });

  // 3) 连线
  ensureEdge(obj, dirId, splitId, 'docs');
  ensureEdge(obj, splitId, embId, 'chunks');
  ensureEdge(obj, embId, vsId, 'embeddings');
  ensureEdge(obj, chatId, miniId, 'question');
  ensureEdge(obj, vsId, miniId, 'vectors');
  ensureEdge(obj, miniId, parserId, 'raw_results');
  ensureEdge(obj, parserId, promptId, 'context');
  ensureEdge(obj, chatId, promptId, 'input');
  ensureEdge(obj, promptId, llmId, 'prompt_text');
  ensureEdge(obj, llmId, postId, 'answer');

  // 移除可能的旧直连
  removeEdge(obj, embId, miniId);

  if (!obj.langflow_version) obj.langflow_version = '61a323d97ded7132c54c97d18e522f69dc6a5fed';
  if (!obj.name) obj.name = 'miniob_rag_model';

  writeJson(outPath, obj);
  console.log(`已生成: ${outPath}`);
}

main();
