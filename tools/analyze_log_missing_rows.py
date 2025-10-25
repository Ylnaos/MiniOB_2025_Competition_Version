#!/usr/bin/env python3
"""
日志分析工具：抽取批量 INSERT 语句并统计缺失数据段。

用法示例：
    python3 tools/analyze_log_missing_rows.py --log build/bin/observer.log.20251025 --table T_P789_A --expected-total 32375

脚本会扫描包含 `INSERT INTO <table> VALUES ...` 的日志行，
统计总插入行数、NULL 行数量、缺失的 id 序列等信息，并输出到终端或指定文件。
"""

import argparse
import re
from collections import Counter
from pathlib import Path
from typing import Iterable, Iterator, List, Optional, Tuple


INSERT_PATTERN_TEMPLATE = r"receive command\(size=\d+\):\s+INSERT INTO {table}\s+VALUES\s+(?P<values>.+);"
VALUE_PATTERN = re.compile(r"\(([^)]+)\)")


def parse_values(values_str: str) -> Iterator[Tuple[Optional[int], Optional[str]]]:
    """从 VALUES(...) 字符串中解析 (id, age) 对。

    如果 id 为 NULL，则返回 (None, age)。
    """
    for match in VALUE_PATTERN.finditer(values_str):
        content = match.group(1)
        parts = [p.strip() for p in content.split(",", 1)]
        if len(parts) != 2:
            continue
        id_str, _age_str = parts
        if id_str.upper() == "NULL":
            yield None, parts[1]
        else:
            try:
                yield int(id_str), parts[1]
            except ValueError:
                continue


def analyze_log(
    log_path: Path,
    table_name: str,
    expected_total: Optional[int],
    limit_missing: int,
) -> Tuple[int, int, int, List[int], Counter]:
    """扫描日志，返回 (总行数, NULL 行数, 非 NULL 行数, 缺失 id 列表, 每条语句行数分布)。"""
    insert_pattern = re.compile(
        INSERT_PATTERN_TEMPLATE.format(table=re.escape(table_name)), re.IGNORECASE
    )

    total_rows = 0
    null_rows = 0
    ids = set()
    per_stmt_counter: Counter[int] = Counter()

    with log_path.open("r", encoding="utf-8", errors="ignore") as fp:
        for line in fp:
            match = insert_pattern.search(line)
            if not match:
                continue
            values_str = match.group("values")
            stmt_rows = 0
            for id_value, _ in parse_values(values_str):
                total_rows += 1
                stmt_rows += 1
                if id_value is None:
                    null_rows += 1
                else:
                    ids.add(id_value)
            per_stmt_counter[stmt_rows] += 1

    non_null_rows = total_rows - null_rows
    missing_ids: List[int] = []

    if expected_total is not None:
        expected_ids = set(range(1, expected_total + 1))
        missing_ids = sorted(expected_ids - ids)
        if limit_missing > 0 and len(missing_ids) > limit_missing:
            missing_ids = missing_ids[:limit_missing]

    return total_rows, null_rows, non_null_rows, missing_ids, per_stmt_counter


def format_counter(counter: Counter) -> str:
    """把 Counter 展开成 `数量 x 行数` 的可读形式。"""
    if not counter:
        return "无插入语句"
    parts = []
    for rows, cnt in sorted(counter.items()):
        parts.append(f"{cnt} 次语句包含 {rows} 行")
    return "；".join(parts)


def main() -> None:
    parser = argparse.ArgumentParser(description="批量分析 MiniOB 日志中的 INSERT 行。")
    parser.add_argument("--log", required=True, help="日志文件路径，如 build/bin/observer.log.20251025")
    parser.add_argument("--table", required=True, help="目标表名，例如 T_P789_A")
    parser.add_argument(
        "--expected-total",
        type=int,
        default=None,
        help="期望的非 NULL id 上限（用于检测缺失 id），例如 32375",
    )
    parser.add_argument(
        "--limit-missing",
        type=int,
        default=50,
        help="输出缺失 id 的最大数量，默认 50，设为 0 则输出全部",
    )
    parser.add_argument(
        "--output",
        help="结果输出到文件（默认打印到终端）。",
    )

    args = parser.parse_args()

    log_path = Path(args.log)
    if not log_path.exists():
        raise FileNotFoundError(f"日志文件不存在：{log_path}")

    total_rows, null_rows, non_null_rows, missing_ids, counter = analyze_log(
        log_path=log_path,
        table_name=args.table,
        expected_total=args.expected_total,
        limit_missing=args.limit_missing if args.limit_missing >= 0 else 0,
    )

    lines: List[str] = []
    lines.append(f"日志文件: {log_path}")
    lines.append(f"目标表: {args.table}")
    lines.append(f"解析到的 INSERT 总行数: {total_rows}")
    lines.append(f"NULL 行数: {null_rows}")
    lines.append(f"非 NULL 行数: {non_null_rows}")
    lines.append(f"INSERT 语句行数分布: {format_counter(counter)}")

    if args.expected_total is not None:
        lines.append(f"期望非 NULL id 上限: {args.expected_total}")
        if missing_ids:
            total_missing = (
                len(missing_ids)
                if args.limit_missing == 0
                else "超过限制" if len(missing_ids) == args.limit_missing else len(missing_ids)
            )
            lines.append(f"缺失 id 数量: {total_missing}")
            lines.append(f"缺失 id 列表(截断显示): {missing_ids}")
        else:
            lines.append("未检测到缺失 id（或未提供期望范围）。")

    output_text = "\n".join(lines)

    if args.output:
        Path(args.output).write_text(output_text, encoding="utf-8")
    else:
        print(output_text)


if __name__ == "__main__":
    main()
