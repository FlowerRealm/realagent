#!/bin/sh
# 格式化入口。--check 只校验不改，退出码非零表示有文件不合规。
# 单一真相是 .clang-format；Ctrl+S、AI 改动、pre-commit 全走这里。
set -eu
cd "$(git rev-parse --show-toplevel)"

CF=${CLANG_FORMAT:-clang-format}
command -v "$CF" >/dev/null || { echo "clang-format 未找到，设 CLANG_FORMAT 指定路径" >&2; exit 127; }

check=0
if [ "${1:-}" = --check ]; then check=1; shift; fi

if [ $# -gt 0 ]; then
    list=$(printf '%s\n' "$@")
else
    list=$(git ls-files -- 'core/*.cpp' 'core/*.hpp')
fi

bad=
for f in $list; do
    [ -f "$f" ] || continue
    if [ "$check" = 1 ]; then
        "$CF" --dry-run --Werror "$f" 2>/dev/null || bad="$bad$f
"
    else
        "$CF" -i "$f"
    fi
done

if [ -n "$bad" ]; then
    echo "格式不合规："
    printf '  %s\n' $bad
    echo "跑 \`make fmt\` 修复。"
    exit 1
fi
