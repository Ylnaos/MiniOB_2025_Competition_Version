#!/usr/bin/env python3
"""
MiniOB ann-benchmarks runner (PyMySQL-based)

This script loads a vector dataset, inserts it into MiniOB via PyMySQL,
optionally creates a (no-op) scalar index, and runs top-k ANN queries using
ORDER BY vector distance functions (L2_DISTANCE/COSINE_DISTANCE/INNER_PRODUCT).

Acceptance goals (subject to dataset size/config):
- Finish end-to-end within 10 minutes (excluding dataset download).
- ANN QPS >= target and recall >= 0.90 (validate via ground truth if provided).
- Memory: run MiniOB with buffer-pool cap (e.g., `-n 800000000`) to stay <= 1GB.

Usage example:
  python tools/miniob_annbench.py \
    --host 127.0.0.1 --port 6789 --table vecs \
    --metric l2 --k 10 --batch-size 1000 \
    --train data/sift-128-train.npy --test data/sift-128-test.npy \
    --truth data/sift-128-truth.npy

Dataset formats supported:
  - .npy (NumPy array) for train, test, and truth (truth as int64 indices).
  - .h5/.hdf5 files with datasets named 'train', 'test', and 'neighbors' (optional).

Note:
  - This runner uses full-scan top-k using ORDER BY; it exercises MiniOB vector
    functions and client protocol. It does not depend on a DB-side ANN index.
  - For maximum throughput, ensure MiniOB runs in MySQL protocol: `-P mysql`.
  - For PyMySQL compatibility, no authentication is required.
"""

import argparse
import os
import time
import math
import sys
from typing import Tuple, Optional

def _lazy_imports():
    global np, pymysql, h5py
    import numpy as np
    import pymysql
    try:
        import h5py  # optional
    except Exception:
        h5py = None
    return np, pymysql, h5py


def load_array(path: str, key: Optional[str] = None):
    np, _, h5py = _lazy_imports()
    ext = os.path.splitext(path)[1].lower()
    if ext == '.npy':
        return np.load(path)
    if ext in ('.h5', '.hdf5'):
        if h5py is None:
            raise RuntimeError('h5py not available to load hdf5 file: %s' % path)
        with h5py.File(path, 'r') as f:
            if key is None:
                # common default keys
                for cand in ('train', 'test', 'neighbors'):
                    if cand in f:
                        return f[cand][...]
                # fall back to first dataset
                for name in f:
                    try:
                        return f[name][...]
                    except Exception:
                        continue
                raise KeyError('no dataset found in %s' % path)
            return f[key][...]
    raise ValueError('unsupported dataset format: %s' % ext)


def ensure_table(conn, table: str, dim: int):
    cur = conn.cursor()
    # drop and create
    try:
        cur.execute(f"drop table {table}")
    except Exception:
        pass
    cur.execute(f"create table {table} (id int, vec vectors)")
    conn.commit()


def insert_vectors(conn, table: str, X, batch_size: int = 1000, start_id: int = 0):
    np, _, _ = _lazy_imports()
    cur = conn.cursor()
    n, d = X.shape
    def vec_lit(row):
        # compact JSON-like literal: [0.1,0.2,...]
        return '[' + ','.join(str(float(v)) for v in row) + ']'
    total = 0
    for i in range(0, n, batch_size):
        j = min(i + batch_size, n)
        vals = []
        for idx in range(i, j):
            vid = start_id + idx
            vals.append(f"({vid}, {conn.escape(vec_lit(X[idx]))})")
        sql = f"insert into {table} values " + ','.join(vals)
        cur.execute(sql)
        total += (j - i)
        if (i // batch_size) % 20 == 0:
            conn.commit()
    conn.commit()
    return total


def metric_to_func(metric: str) -> str:
    m = metric.lower()
    if m in ('l2', 'euclidean'): return 'L2_DISTANCE'
    if m in ('cosine', 'angular'): return 'COSINE_DISTANCE'
    if m in ('ip', 'inner', 'dot'): return 'INNER_PRODUCT'
    raise ValueError('unsupported metric: %s' % metric)


def run_queries(conn, table: str, Q, k: int, metric: str) -> Tuple[list, float]:
    np, _, _ = _lazy_imports()
    cur = conn.cursor()
    func = metric_to_func(metric)
    results = []
    t0 = time.time()
    for i in range(Q.shape[0]):
        q = '[' + ','.join(str(float(v)) for v in Q[i]) + ']'
        sql = f"select id, {func}(vec, {conn.escape(q)}) as dist from {table} order by dist limit {int(k)}"
        cur.execute(sql)
        rows = cur.fetchall()
        # rows are tuples; first col is id
        ids = [int(r[0]) for r in rows]
        results.append(ids)
    dt = time.time() - t0
    return results, dt


def recall_at_k(res, truth, k: int) -> float:
    # truth shape: (nq, >=k), res: list of length nq
    import numpy as np
    nq = len(res)
    hit = 0
    for i in range(nq):
        gt = set(int(x) for x in truth[i][:k])
        got = set(int(x) for x in res[i])
        hit += len(gt & got) / float(k)
    return hit / float(nq)


def main():
    _lazy_imports()
    ap = argparse.ArgumentParser(description='MiniOB ann-benchmarks runner (PyMySQL)')
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=6789)
    ap.add_argument('--user', default='root')
    ap.add_argument('--password', default='')
    ap.add_argument('--table', default='vecs')
    ap.add_argument('--metric', default='l2', choices=['l2','euclidean','cosine','angular','ip','inner','dot'])
    ap.add_argument('--k', type=int, default=10)
    ap.add_argument('--batch-size', type=int, default=1000)
    ap.add_argument('--train', required=True, help='.npy or .h5 path for base vectors')
    ap.add_argument('--test', required=True, help='.npy or .h5 path for query vectors')
    ap.add_argument('--truth', help='.npy or .h5 path for ground-truth nn indices (optional)')
    ap.add_argument('--truth-key', default=None, help='dataset key for truth in h5 (default: neighbors)')
    args = ap.parse_args()

    np, pymysql, _ = _lazy_imports()
    X = load_array(args.train)
    Q = load_array(args.test)
    if X.ndim != 2 or Q.ndim != 2:
        print('Expected 2D arrays; got shapes:', X.shape, Q.shape)
        return 2
    if X.shape[1] != Q.shape[1]:
        print('Dim mismatch: train dim %d vs test dim %d' % (X.shape[1], Q.shape[1]))
        return 2

    truth = None
    if args.truth:
        truth = load_array(args.truth, key=args.truth_key)

    print('Connecting to MiniOB at %s:%d ...' % (args.host, args.port))
    conn = pymysql.connect(host=args.host, port=args.port, user=args.user, password=args.password, database='sys', charset='utf8')
    try:
        print('Preparing table %s (dim=%d)...' % (args.table, X.shape[1]))
        ensure_table(conn, args.table, X.shape[1])

        print('Inserting %d vectors in batches of %d ...' % (X.shape[0], args.batch_size))
        t0 = time.time()
        n_ins = insert_vectors(conn, args.table, X.astype(np.float32, copy=False), batch_size=args.batch_size, start_id=0)
        t_ins = time.time() - t0
        print('Inserted %d vectors in %.2fs (%.1f kv/s)' % (n_ins, t_ins, n_ins / max(t_ins, 1e-9)))

        print('Running %d queries, k=%d, metric=%s ...' % (Q.shape[0], args.k, args.metric))
        res, t_q = run_queries(conn, args.table, Q.astype(np.float32, copy=False), args.k, args.metric)
        qps = len(res) / max(t_q, 1e-9)
        print('Query time: %.2fs, QPS: %.2f' % (t_q, qps))

        rec = None
        if truth is not None:
            # standardize shape
            if truth.ndim == 2 and truth.shape[1] < args.k:
                print('Truth has fewer than k neighbors; recall computed on available columns')
                k_eval = truth.shape[1]
            else:
                k_eval = args.k
            rec = recall_at_k(res, truth, k_eval)
            print('Recall@%d: %.4f' % (k_eval, rec))

        # final report (machine-readable)
        summary = {
            'dim': int(X.shape[1]),
            'train': int(X.shape[0]),
            'test': int(Q.shape[0]),
            'k': int(args.k),
            'metric': args.metric,
            'insert_seconds': float(t_ins),
            'query_seconds': float(t_q),
            'qps': float(qps),
            'recall': float(rec) if rec is not None else None,
        }
        print('SUMMARY:', summary)
    finally:
        try:
            conn.close()
        except Exception:
            pass

    return 0


if __name__ == '__main__':
    sys.exit(main())

