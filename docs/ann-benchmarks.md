MiniOB ann-benchmarks Runner (PyMySQL)

Overview
- This repo includes a helper script to evaluate MiniOB vector search with ann-benchmarks-style workloads using PyMySQL.
- It exercises vector distance functions (L2_DISTANCE/COSINE_DISTANCE/INNER_PRODUCT) via SQL ORDER BY and measures QPS/recall.
- It does not implement a DB-side ANN index; it focuses on protocol and functional correctness. You can iterate later with custom vector indexes.

Prerequisites
- Build and run MiniOB server in MySQL protocol:
  - Example: `build.sh` then start: `./build/bin/observer -P mysql -n 800000000`
    - `-n 800000000` caps buffer pool memory < 1 GB.
    - Default port is `6789`.
- Python 3 with packages: `pymysql`, `numpy` (and optionally `h5py`).

Runner usage
- Script: `tools/miniob_annbench.py`
- Example (NumPy arrays):
  - `python tools/miniob_annbench.py --host 127.0.0.1 --port 6789 --table vecs --metric l2 --k 10 --batch-size 1000 --train data/sift-128-train.npy --test data/sift-128-test.npy --truth data/sift-128-truth.npy`
- Example (HDF5):
  - `python tools/miniob_annbench.py --train data/dataset.h5 --test data/dataset.h5 --truth data/dataset.h5 --truth-key neighbors`

What it does
- Drops and creates table: `create table vecs (id int, vec vectors)`.
- Inserts training vectors in batches.
- Runs queries: `select id, L2_DISTANCE(vec, [q]) as dist from vecs order by dist limit k` (or COSINE/INNER_PRODUCT).
- Computes QPS and Recall@k (if ground truth provided).

Notes
- Ensure vector literals are passed as strings. PyMySQL escapes them; MiniOB casts strings to VECTORS in distance functions.
- Column metadata returned by MiniOB is string-typed; PyMySQL still parses values fine.
- For stability, the server clears `CLIENT_DEPRECATE_EOF` and returns EOF at the end of resultsets; PyMySQL handles both EOF/OK.

Targets
- End-to-end run within 10 minutes depends on dataset scale and environment. Use smaller datasets first (e.g., 100k vectors) to validate.
- Memory <= 1 GB: set `-n` at server start; avoid large in-memory indexes.

Next steps (optional)
- Add vector indexes and pushdown top-k to increase QPS.
- Extend MySQL column metadata typing to return numeric types for better client-side decoding.

