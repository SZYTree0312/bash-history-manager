#!/usr/bin/env bash
# bhm 自测：造 fixture → 跑命令 → 断言。
# 用法：先 make（或 cl）编出 bhm / bhm.exe，再 bash tests/run.sh
set -u
cd "$(dirname "$0")/.."

# 默认用仓库里的 ./bhm（或 ./bhm.exe）；也可以用 BHM= 指定别处的二进制：
#   BHM=/usr/local/bin/bhm bash tests/run.sh
if [ -n "${BHM:-}" ]; then :
elif [ -x ./bhm ];     then BHM=./bhm
elif [ -x ./bhm.exe ]; then BHM=./bhm.exe
else
  echo "找不到 bhm，先编译：make  或  cl /std:c++17 /EHsc /Fe:bhm bash_history_manager.cpp"
  exit 1
fi
echo "被测二进制: $BHM"

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL $1"; }
check() {
  if [ "$2" = "$3" ]; then ok "$1"; else bad "$1（期望 [$3]，实际 [$2]）"; fi
}

WORK=$(mktemp -d ./tmp.bhmtest.XXXXXX 2>/dev/null || mktemp -d)
trap 'rm -rf "$WORK"' EXIT
echo "测试目录: $WORK"

# fixture 1：bash 扩展格式，带重复和一条带密码的
cat > "$WORK/bash_history" <<'EOF'
#1728000000
ls -la
#1728000100
git status
#1728000200
ls -la
#1728000300
docker ps
#1728000400
mysql -uroot -pHunter2Secret
EOF

echo "== list =="
OUT=$($BHM --file "$WORK/bash_history" list)
check "list 列出 5 行" "$(echo "$OUT" | grep -c '^\[')" "5"
check "list 报总条数 5" "$(echo "$OUT" | tail -1)" "共 5 条"
OUT=$($BHM --file "$WORK/bash_history" list -n 2)
check "list -n 2 只列 2 行" "$(echo "$OUT" | grep -c '^\[')" "2"

echo "== search =="
check "search git 命中 1 条" "$($BHM --file "$WORK/bash_history" search git | grep -c '^\[')" "1"
check "search -i 忽略大小写" "$($BHM --file "$WORK/bash_history" search DOCKER -i | grep -c '^\[')" "1"
check "search 无匹配有提示" "$($BHM --file "$WORK/bash_history" search nothere)" "没有匹配「nothere」的命令"
check "search --regex" "$($BHM --file "$WORK/bash_history" search --regex 'git (status|push)' | grep -c '^\[')" "1"

echo "== info =="
INFO=$($BHM --file "$WORK/bash_history" info)
echo "$INFO" | grep -q 'bash 扩展格式' && ok "info 识别 bash 格式" || bad "info 格式识别"
echo "$INFO" | grep -q '总条数   : 5（去重后 4）' && ok "info 统计去重" || bad "info 统计去重"
echo "$INFO" | grep -q '时间范围 : 2024' && ok "info 时间范围" || bad "info 时间范围"

echo "== delete =="
OUT=$($BHM --file "$WORK/bash_history" delete 'mysql' --yes)
check "delete 报删除 1 条" "$(echo "$OUT" | grep -c '已删除 1 条')" "1"
check "删除后剩 4 条" "$($BHM --file "$WORK/bash_history" list | tail -1)" "共 4 条"
if grep -q 'Hunter2Secret' "$WORK/bash_history"; then bad "敏感命令还在文件里"; else ok "敏感命令已从文件移除"; fi
check "时间戳行同步移除" "$(grep -c '^#' "$WORK/bash_history")" "4"
BAK_N=$(ls "$HOME/.bhm/backups" 2>/dev/null | wc -l)
check "自动生成了备份" "$([ "${BAK_N:-0}" -ge 1 ] && echo yes || echo no)" "yes"

# 非交互且不加 --yes：拒绝动手，文件不动
cp "$WORK/bash_history" "$WORK/before"
$BHM --file "$WORK/bash_history" delete git < /dev/null > /dev/null 2>&1
check "非交互无 --yes 拒绝删除" "$?" "2"
if cmp -s "$WORK/before" "$WORK/bash_history"; then ok "拒绝后文件未动"; else bad "拒绝后文件被改了"; fi

OUT=$($BHM --file "$WORK/bash_history" delete zzzz --yes)
check "无匹配不动文件" "$OUT" "没有匹配「zzzz」的命令，未改动任何内容"

echo "== export =="
$BHM --file "$WORK/bash_history" export -o "$WORK/out.csv" > /dev/null
check "CSV 行数 = 4 条 + 表头" "$(wc -l < "$WORK/out.csv")" "5"
if head -c 3 "$WORK/out.csv" | od -An -tx1 | grep -q 'ef bb bf'; then ok "CSV 带 BOM"; else bad "CSV 缺 BOM"; fi

echo "== clear / restore =="
OUT=$($BHM --file "$WORK/bash_history" clear --yes)
check "clear 报清空 4 条" "$(echo "$OUT" | grep -c '已清空 4 条')" "1"
check "clear 后文件为空" "$(wc -c < "$WORK/bash_history")" "0"
LAST=$(ls -t "$HOME/.bhm/backups" 2>/dev/null | head -1)
OUT=$($BHM --file "$WORK/bash_history" restore "$HOME/.bhm/backups/$LAST" --yes)
check "restore 报恢复 4 条" "$(echo "$OUT" | grep -c '已恢复 4 条')" "1"
check "restore 后回到 4 条" "$($BHM --file "$WORK/bash_history" list | tail -1)" "共 4 条"

echo "== zsh 格式 =="
cat > "$WORK/zsh_history" <<'EOF'
: 1728000000:0;ls -la
: 1728000100:2;git status
: 1728000200:0;ls -la
EOF
check "zsh 解析出 3 条" "$($BHM --file "$WORK/zsh_history" list | tail -1)" "共 3 条"
$BHM --file "$WORK/zsh_history" info | grep -q 'zsh 扩展格式' && ok "info 识别 zsh 格式" || bad "zsh 格式识别"
$BHM --file "$WORK/zsh_history" delete git --yes > /dev/null
check "zsh 删除后剩 2 条" "$($BHM --file "$WORK/zsh_history" list | tail -1)" "共 2 条"
check "zsh 删除后格式保持" "$(grep -c '^: ' "$WORK/zsh_history")" "2"

echo "== 纯文本格式 =="
printf 'ls\ngit status\nls\n' > "$WORK/plain"
check "纯文本解析 3 条" "$($BHM --file "$WORK/plain" list | tail -1)" "共 3 条"

echo
echo "通过 $PASS 项，失败 $FAIL 项"
[ "$FAIL" -eq 0 ]
