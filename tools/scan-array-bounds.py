#!/usr/bin/env python3
# tools/scan-array-bounds.py —— BUG-14 类隐患静态扫描(纯文本启发式,零依赖)。
#
# BUG-14:s_keys 声明 [10](r8 十键遗留),循环却以 MPD_KEY_COUNT=15 限界,
# 编译器只在越界侧报警告,声明侧永远沉默 —— UB 吃了列表页三个版本。
# 本扫描把同类形态在代码评审前抓出来:
#   V1  文件作用域字面量尺寸数组,被以"更大的常量/字面量"限界的 for 循环索引
#   V1b 非 static 全局字面量尺寸数组(单源化提醒,人工复核)
#   V2  字面量尺寸字符数组,被无界写函数(strcpy/strcat/sprintf/gets)填充
# 扫描范围:第一方代码(main/ 与 components/bsp/)。退出码 0=干净,1=发现疑似。
#
# 诚实边界:正则启发式,非完整数据流分析;宁可多报(标记"人工复核"),
# 不漏报已知的 BUG-14 形态(含 *name[N] 无空格指针数组)。
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATTERNS = ["main/**/*.c", "main/**/*.h",
            "components/bsp/**/*.c", "components/bsp/**/*.h"]
DECL = re.compile(r"^static\s+[\w\s]+?[\s\*](\w+)\s*\[\s*(\d+)\s*\]")
GLOBAL_DECL = re.compile(r"^[a-zA-Z_][\w\s\*]*?\s(\w+)\s*\[\s*(\d+)\s*\]")
FOR_BOUND = re.compile(
    r"for\s*\([^;]*;\s*[\w\s\*]*(\w+)\s*=\s*[\w\.]+\s*;\s*"
    r"(\w+)\s*(<|<=)\s*([\w]+)")
UNBOUND_WRITE = re.compile(
    r"\b(strcpy|strcat|sprintf|gets)\s*\(\s*(\w+)\s*,")


def first_party_files():
    seen = set()
    for pat in PATTERNS:
        for f in glob.glob(os.path.join(ROOT, pat), recursive=True):
            rel = os.path.relpath(f, ROOT)
            if rel not in seen and os.path.isfile(f):
                seen.add(rel)
    return sorted(seen)


def loop_bounds_for(lines, name):
    """name[var] 每个索引变量所属 for 循环的 (bound, inclusive) 集合。"""
    use = re.compile(r"\b" + re.escape(name) + r"\s*\[\s*([A-Za-z_]\w*)\s*\]")
    bounds = {}
    for j, line in enumerate(lines):
        for um in use.finditer(line):
            var = um.group(1)
            for k in range(j, max(0, j - 40), -1):
                fb = FOR_BOUND.search(lines[k])
                if fb and fb.group(1) == var and fb.group(2) == var:
                    bounds.setdefault(var, set()).add(
                        (fb.group(4), fb.group(3) == "<="))
                    break
    return bounds


def main():
    files = first_party_files()
    srcs = {f: open(os.path.join(ROOT, f), errors="replace").read().split("\n")
            for f in files}

    defines = {}
    for lines in srcs.values():
        for line in lines:
            m = re.match(r"#define\s+(\w+)\s+(\d+)", line)
            if m:
                defines[m.group(1)] = int(m.group(2))

    findings = []
    static_arrays = {}   # name -> (file, lineno, size) 供 V2 引用
    for f in files:
        for i, line in enumerate(srcs[f]):
            m = DECL.match(line)
            if m:
                name, size = m.group(1), int(m.group(2))
                static_arrays[name] = (f, i + 1, size)
                # ---- V1: 循环限界(常量或字面量)达到声明尺寸即越界 ----
                for var, bs in sorted(loop_bounds_for(srcs[f], name).items()):
                    for bound, inclusive in sorted(bs):
                        limit = None
                        if bound.isdigit():
                            limit = int(bound) - (0 if inclusive else 1)
                        elif bound in defines:
                            limit = defines[bound] - (0 if inclusive else 1)
                        if limit is not None and limit >= size:
                            findings.append(
                                f"V1 {f}:{i + 1}: {name}[{size}] indexed by "
                                f"loop '{var} <{bound}' (max index {limit}) "
                                f"— overflow risk; 声明尺寸改为与限界常量同源")
            elif GLOBAL_DECL.match(line):
                findings.append(
                    f"V1b {f}:{i + 1}: global "
                    f"{GLOBAL_DECL.match(line).group(1)}["
                    f"{GLOBAL_DECL.match(line).group(2)}] literal size — "
                    f"尺寸单源为常量(人工复核)")

    # ---- V2: 无界写函数填充任何已知 static 字面量数组 ----
    for f in files:
        for i, line in enumerate(srcs[f]):
            for um in UNBOUND_WRITE.finditer(line):
                name = um.group(2)
                if name in static_arrays:
                    where = static_arrays[name]
                    findings.append(
                        f"V2 {f}:{i + 1}: {um.group(1)}() writes "
                        f"{name}[{where[2]}] (declared {where[0]}:"
                        f"{where[1]}) unbounded — 改用 snprintf/strncpy "
                        f"+ sizeof")

    if findings:
        print("疑似 BUG-14 类隐患(逐条人工核实,误报需收紧扫描器):")
        for x in findings:
            print("  " + x)
        sys.exit(1)
    print(f"array-bounds scan: {len(files)} files clean "
          f"(V1 loop-bound overflow / V1b global literal / V2 unbounded write)")


if __name__ == "__main__":
    main()
