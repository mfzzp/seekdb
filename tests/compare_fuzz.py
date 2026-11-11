"""Compare two fuzz result files: ids must match exactly, distances to 1e-9."""

import json
import math
import sys


def compare(a, b):
    if "error" in a or "error" in b:
        if a.get("error") != b.get("error"):
            return f"error mismatch: {a.get('error')} vs {b.get('error')}"
        return None
    left, right = a["result"], b["result"]
    if left.get("columns") != right.get("columns"):
        return f"columns {left.get('columns')} vs {right.get('columns')}"
    lrows, rrows = left.get("rows", []), right.get("rows", [])
    if len(lrows) != len(rrows):
        return f"row count {len(lrows)} vs {len(rrows)}"
    for index, (lrow, rrow) in enumerate(zip(lrows, rrows)):
        if len(lrow) != len(rrow):
            return f"row {index} width"
        for column, (lval, rval) in enumerate(zip(lrow, rrow)):
            if isinstance(lval, float) or isinstance(rval, float):
                if not (isinstance(lval, (int, float)) and isinstance(rval, (int, float))):
                    return f"row {index} col {column} type {lval!r} vs {rval!r}"
                if abs(float(lval) - float(rval)) > 1e-9:
                    return f"row {index} col {column} value {lval} vs {rval}"
            elif lval != rval:
                return f"row {index} col {column} value {lval!r} vs {rval!r}"
    return None


def main():
    left = json.load(open(sys.argv[1]))
    right = json.load(open(sys.argv[2]))
    if len(left) != len(right):
        print(f"FAIL count {len(left)} vs {len(right)}")
        return 1
    failures = 0
    for a, b in zip(left, right):
        problem = compare(a, b)
        if problem:
            failures += 1
            if failures <= 5:
                print(f"FAIL {a['sql']} [{a['mode']}]: {problem}")
    print(f"{len(left) - failures}/{len(left)} fuzz queries identical")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
