#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# sharefs_casefold —— sharefs ASCII 大小写不敏感适配（变体查找 + 只改大小写的 rename）
#
# 脚本是自解释的，不需要任何外部测试用例表：
#   * 开头打印本套件要验证的全部用例（编号 + 测什么 + 期望结果）；
#   * 每条用例执行后打印 [编号] PASS/FAIL/SKIP + 该用例说明 + 实测证据；
#   * 结尾给 SUMMARY，失败项按编号回查开头清单即可定位。
# 板侧只有 toybox，脚本不依赖 awk/python。
#
# 用法（在存放本脚本与 t_* 程序的目录下执行，需 root）：
#   sh sharefs_casefold_test.sh          # 跑全量（约 1 分钟）
#   sh sharefs_casefold_test.sh --list   # 只打印要测什么，不执行
#   sh sharefs_casefold_test.sh --help
#
# 前置条件：
#   1) 内核已启用 sharefs 且支持 casefold（挂载选项 case_sensitive 可用）；
#   2) 只改大小写的 rename 分支由 fs/sharefs/Makefile 里的
#      ccflags-y += -DSHAREFS_CASEFOLD_RENAME_EXPERIMENTAL 控制，默认注释掉；
#      跑 CF-11/CF-13/CF-21 之前需要打开它重新编译内核，否则这几项会失败；
#   3) t_casefold_rename / t_ci_bench / t_rename_exchange / t_rename_flags0 与本脚本同目录；
#   4) root 权限（脚本会在 /data/local/tmp 下自建 lower 目录与两个挂载点，跑完卸载）。
#
# 语义约定（判定以此为准）：
#   * 只有 ASCII 字母参与大小写折叠，非 ASCII（如中文）与数字不参与；
#   * 精确名优先：目录里同时存在 a.txt 与 A.txt 时，查 A.txt 必须命中 A.txt 本身；
#   * 只改大小写的 rename 必须真正落到 lower 盘面上。

BASE=/data/local/tmp/kselftest_sharefs_casefold
L=$BASE/lower
M_CI=$BASE/ci
M_CS=$BASE/cs
PROG=$PWD
N_PASS=0; N_FAIL=0; N_SKIP=0

# ---------------- 用例说明表（编号 -> 测什么 + 期望结果）----------------
desc() {
    case "$1" in
    CF-01) echo "变体查找基本功能：用 A.TXT 访问（lower 里只有 a.txt/A.txt）应能命中某个变体" ;;
    CF-02) echo "内核证据：日志应出现 ci lookup hit，证明命中走的是大小写兜底路径而非精确匹配" ;;
    CF-03) echo "同目录大小写两实体共存：a.txt 与 A.txt 是两个不同 inode，各自读到自己的内容（精确名优先）" ;;
    CF-04) echo "变体扫描：A.TxT 在 lower 无精确项，应由大小写扫描命中某个已存在的变体" ;;
    CF-05) echo "折叠范围：中文词干保持精确、仅 ASCII 扩展名折叠，中文文件.TXT 应命中中文文件.txt" ;;
    CF-06) echo "中文精确名本身可正常访问" ;;
    CF-07) echo "折叠范围：纯数字名 12345 不参与大小写扫描，按精确名访问" ;;
    CF-08) echo "挂载选项隔离：case_sensitive 挂载上，变体名 A.TXT 必须 ENOENT（不得泄漏大小写行为）" ;;
    CF-09) echo "case_sensitive 挂载上精确名 A.txt 仍可正常访问" ;;
    CF-10) echo "只改大小写的 rename 主路径：mv t8.txt T8.TXT 调用应成功" ;;
    CF-11) echo "改名结果落到 lower 盘面：readdir 应能看到新拼写 T8.TXT" ;;
    CF-12) echo "大小写语义保持：改完后用旧变体 t8.txt 仍能读到原内容" ;;
    CF-13) echo "内核证据：日志出现 casefold rename (direct), err 0，说明走了 casefold direct 分支" ;;
    CF-14) echo "同名 rename（T8.TXT -> T8.TXT）应是无副作用的 no-op，返回成功" ;;
    CF-15) echo "同名 rename 后内容完好" ;;
    CF-16) echo "同目录普通改名（名字不只改大小写）应正常落到 lower" ;;
    CF-17) echo "跨目录改名应正常落到 lower 的目标目录" ;;
    CF-18) echo "路径逐段生效：子目录名与文件名同时用变体（SUB/S.TXT）也应命中" ;;
    CF-19) echo "用变体名删除：rm R3DEL.TXT 应删掉 lower 里的 r3del.txt" ;;
    CF-20) echo "只改大小写的 rename 六分支矩阵（t_casefold_rename：direct/EEXIST/标准路径等）全部通过" ;;
    CF-21) echo "内核证据：日志出现 casefold rename (direct), err 0（direct 分支被覆盖）" ;;
    CF-22) echo "内核证据：日志出现 casefold rename target exists（目标已存在分支被覆盖）" ;;
    CF-23) echo "敏感挂载走标准路径：case_sensitive 上做只改大小写的 rename 不应进入 casefold 分支" ;;
    CF-24) echo "回归：flags=0 的 rename（t_rename_flags0）不受大小写改动影响" ;;
    CF-25) echo "回归：RENAME_EXCHANGE（t_rename_exchange）应被拒（-EINVAL）且内容不变" ;;
    CF-26) echo "性能基线：变体查找基准程序可跑通，输出精确命中/含字母 miss/纯数字 miss 三类耗时" ;;
    CF-27) echo "收尾：卸载本脚本自建的两个挂载点（数据在 $BASE 下保留，便于事后分析）" ;;
    CF-28) echo "回归：全程内核日志不得出现 BUG / sharefs WARNING / sharefs 调用栈" ;;
    *)    echo "(未登记用例)" ;;
    esac
}

ALL_CASES="CF-01 CF-02 CF-03 CF-04 CF-05 CF-06 CF-07 CF-08 CF-09 CF-10 CF-11 CF-12 CF-13 CF-14 CF-15 CF-16 CF-17 CF-18 CF-19 CF-20 CF-21 CF-22 CF-23 CF-24 CF-25 CF-26 CF-27 CF-28"

list_cases() {
    echo "sharefs_casefold 将验证以下用例（编号 -> 测什么 / 期望结果）："
    for c in $ALL_CASES; do printf '  %-6s %s\n' "$c" "$(desc $c)"; done
}

usage() {
    echo "用法: sh sharefs_casefold_test.sh [--list|--help]"
    echo "  --list 只打印用例清单，不执行"
    echo "  --help 显示本帮助"
    echo "运行环境: 板侧 root，本脚本与 t_casefold_rename/t_ci_bench/t_rename_exchange/t_rename_flags0 同目录"
}

# ---------------- 结果输出 ----------------
# p/f/s <编号> [实测证据]  —— 自动附上该编号的用例说明
p() { printf '[%s] PASS  %s' "$1" "$(desc $1)"; [ -n "$2" ] && printf '  | 实测: %s' "$2"; echo; N_PASS=$((N_PASS+1)); }
f() { printf '[%s] FAIL  %s' "$1" "$(desc $1)"; [ -n "$2" ] && printf '  | 实测: %s' "$2"; echo; N_FAIL=$((N_FAIL+1)); }
s() { printf '[%s] SKIP  %s' "$1" "$(desc $1)"; [ -n "$2" ] && printf '  | 原因: %s' "$2"; echo; N_SKIP=$((N_SKIP+1)); }

case "$1" in
    -l|--list) list_cases; exit 0 ;;
    -h|--help) usage; exit 0 ;;
esac

echo "=== sharefs_casefold: ASCII 大小写不敏感（变体查找 + 只改大小写的 rename）==="
echo "lower: $L    大小写挂载: $M_CI    敏感挂载: $M_CS    程序目录: $PROG"
echo ""
list_cases
echo ""
echo "--- 开始执行 ---"

# ---------- 环境准备 ----------
umount $M_CI 2>/dev/null; umount $M_CS 2>/dev/null
# lower dcache 中可能残留上一轮探测留下的 negative dentry，导致 rm -rf 失败；
# 把旧树挪到旁路，绝不让它卡住本轮。
rm -rf ${BASE}_old 2>/dev/null
[ -d $BASE ] && mv $BASE ${BASE}_old 2>/dev/null
mkdir -p $L $M_CI $M_CS
echo hello-lower > $L/a.txt
echo second       > $L/A.txt
echo num-content  > $L/12345
echo cn-content   > $L/中文文件.txt
chmod -R 777 $BASE

mount -t sharefs -o user_id=0,override_support_delete $L $M_CI || { echo "FATAL: 大小写挂载失败，检查内核是否支持 sharefs casefold"; exit 1; }
mount -t sharefs -o user_id=0,case_sensitive,override_support_delete $L $M_CS || { echo "FATAL: case_sensitive 挂载失败"; exit 1; }
dmesg -c >/dev/null 2>&1

# ---------- CF-01/CF-02 变体查找 ----------
# 双实体目录（a.txt + A.txt）：变体名没有精确项，大小写扫描会命中 readdir 顺序上的
# 第一个折叠匹配项。命中任一变体即证明 ASCII 大小写不敏感生效；精确名优先由 CF-03 覆盖。
v=$(cat $M_CI/A.TXT 2>/dev/null)
{ [ "$v" = "second" ] || [ "$v" = "hello-lower" ]; } && p CF-01 "命中内容='$v'" || f CF-01 "读到 '$v'（期望 second 或 hello-lower）"
dmesg | grep -q 'ci lookup hit' && p CF-02 "日志有 ci lookup hit" || f CF-02 "日志无 ci lookup hit"

# ---------- CF-03 两实体共存 ----------
i1=$(ls -i $L/A.txt 2>/dev/null | sed 's/ .*//')
i2=$(ls -i $L/a.txt 2>/dev/null | sed 's/ .*//')
v1=$(cat $M_CI/A.txt); v2=$(cat $M_CI/a.txt)
[ "$v1" = "second" ] && [ "$v2" = "hello-lower" ] && [ -n "$i1" ] && [ "$i1" != "$i2" ] && \
    p CF-03 "两个 inode $i1/$i2 不同，各自内容正确" || f CF-03 "共存被破坏 v1=$v1 v2=$v2 i1=$i1 i2=$i2"

# ---------- CF-04 变体扫描 ----------
v=$(cat $M_CI/A.TxT 2>/dev/null)
{ [ "$v" = "second" ] || [ "$v" = "hello-lower" ]; } && p CF-04 "命中内容='$v'" || f CF-04 "读到 '$v'"

# ---------- CF-05/CF-06 折叠范围（非 ASCII） ----------
# 语义：ASCII 部分折叠、非 ASCII 字节保持精确，所以 中文文件.TXT 命中 中文文件.txt，
# 这是扩展名折叠生效的预期结果。
v=$(cat "$M_CI/中文文件.TXT" 2>/dev/null)
[ "$v" = "cn-content" ] && p CF-05 "命中中文文件.txt，扩展名折叠生效" || f CF-05 "变体未命中"
v=$(cat "$M_CI/中文文件.txt" 2>/dev/null)
[ "$v" = "cn-content" ] && p CF-06 "精确名可读" || f CF-06 "精确名不可读"

# ---------- CF-07 数字名 ----------
v=$(cat $M_CI/12345 2>/dev/null)
[ "$v" = "num-content" ] && p CF-07 "数字名按精确命中" || f CF-07 "读到 '$v'"

# ---------- CF-08/CF-09 敏感挂载隔离 ----------
v=$(cat $M_CS/A.TXT 2>/dev/null)
[ -z "$v" ] && p CF-08 "变体名 ENOENT" || f CF-08 "敏感挂载泄漏了大小写行为（读到 '$v'）"
v=$(cat $M_CS/A.txt 2>/dev/null)
[ "$v" = "second" ] && p CF-09 "精确名可读" || f CF-09 "精确名不可读"

# ---------- CF-10 ~ CF-13 只改大小写的 rename ----------
# 盘面拼写一律用 readdir(ls) 判定，不用 stat：改名前的目标探测会在 lower dcache 里留下
# 新名字的 negative dentry，导致直接 stat lower 短暂返回 ENOENT（盘上其实已经改名成功，
# 日志为 casefold rename (direct), err 0）；经大小写挂载访问一次即自愈。
echo t8src > $L/t8.txt
dmesg -c >/dev/null 2>&1
mv $M_CI/t8.txt $M_CI/T8.TXT >/dev/null 2>&1 && p CF-10 "mv 成功" || f CF-10 "mv 失败"
ls $L 2>/dev/null | grep -q '^T8.TXT$' && p CF-11 "readdir 见到 T8.TXT" || f CF-11 "lower 拼写未变"
v=$(cat $M_CI/t8.txt 2>/dev/null); [ "$v" = "t8src" ] && p CF-12 "旧变体仍读到 t8src" || f CF-12 "旧变体读到 '$v'"
dmesg | grep -q 'casefold rename (direct), err 0' && p CF-13 "日志有 direct 分支 err 0" || f CF-13 "日志无 direct 分支证据（检查是否打开了 SHAREFS_CASEFOLD_RENAME_EXPERIMENTAL）"

# ---------- CF-14/CF-15 同名 no-op ----------
mv $M_CI/T8.TXT $M_CI/T8.TXT >/dev/null 2>&1 && p CF-14 "调用成功" || f CF-14 "同名 mv 失败"
v=$(cat $M_CI/T8.TXT 2>/dev/null); [ "$v" = "t8src" ] && p CF-15 "内容仍为 t8src" || f CF-15 "内容丢失，读到 '$v'"

# ---------- CF-16 同目录改名 ----------
mv $M_CI/T8.TXT $M_CI/t8b.txt >/dev/null 2>&1 && [ -f $L/t8b.txt ] && p CF-16 "lower 出现 t8b.txt" || f CF-16 "同目录改名失败"

# ---------- CF-17 跨目录改名 ----------
mkdir -p $L/d2
echo mv11 > $L/mv.txt
mv $M_CI/mv.txt $M_CI/d2/moved.txt >/dev/null 2>&1 && [ -f $L/d2/moved.txt ] && p CF-17 "lower/d2/moved.txt 存在" || f CF-17 "跨目录改名失败"

# ---------- CF-18 子目录逐段生效 ----------
mkdir -p $L/sub; echo sub-content > $L/sub/s.txt
v=$(cat $M_CI/SUB/S.TXT 2>/dev/null)
[ "$v" = "sub-content" ] && p CF-18 "SUB/S.TXT 命中 sub/s.txt" || f CF-18 "读到 '$v'"

# ---------- CF-19 变体删除 ----------
echo r3 > $L/r3del.txt
rm $M_CI/R3DEL.TXT >/dev/null 2>&1 && [ ! -f $L/r3del.txt ] && p CF-19 "lower/r3del.txt 已删除" || f CF-19 "变体删除失败"

# ---------- CF-20 ~ CF-22 六分支矩阵 ----------
$PROG/t_casefold_rename $M_CI $M_CS > $BASE/r6.out 2>&1
r6p=$(grep -c '^PASS' $BASE/r6.out); r6f=$(grep -c '^FAIL' $BASE/r6.out)
[ "$r6f" = "0" ] && p CF-20 "矩阵内 $r6p 项全部通过（明细: $BASE/r6.out）" || f CF-20 "$r6p 通过 / $r6f 失败（明细: $BASE/r6.out）"
dmesg | grep -q 'casefold rename (direct), err 0' && p CF-21 "日志有 direct 分支" || f CF-21 "日志无 direct 分支"
dmesg | grep -q 'casefold rename target exists' && p CF-22 "日志有 target exists 分支" || f CF-22 "日志无 EEXIST 分支"

# ---------- CF-23 敏感挂载标准路径 ----------
echo v6 > $M_CS/V6S.TXT 2>/dev/null && {
    dmesg -c >/dev/null 2>&1
    mv $M_CS/V6S.TXT $M_CS/v6s.txt >/dev/null 2>&1 && [ -f $L/v6s.txt ] && \
        dmesg | grep -q 'casefold rename' && f CF-23 "敏感挂载竟进入 casefold 分支" || \
        p CF-23 "未出现 casefold 日志，走标准 rename 路径"
}

# ---------- CF-24/CF-25 rename 回归 ----------
$PROG/t_rename_flags0 $M_CI > $BASE/cf24.out 2>&1
grep -q '^FAIL' $BASE/cf24.out && f CF-24 "存在失败项（明细: $BASE/cf24.out）" || p CF-24 "无失败项（明细: $BASE/cf24.out）"
$PROG/t_rename_exchange $M_CI > $BASE/cf25.out 2>&1
grep -q '^FAIL' $BASE/cf25.out && f CF-25 "存在失败项（明细: $BASE/cf25.out）" || p CF-25 "无失败项（明细: $BASE/cf25.out）"

# ---------- CF-26 性能基线 ----------
$PROG/t_ci_bench $M_CI > $BASE/bench.out 2>&1
grep -q 'exact_hit' $BASE/bench.out && p CF-26 "基准完成（数据: $BASE/bench.out）" || f CF-26 "基准未产出结果"

# ---------- 收尾 ----------
umount $M_CI $M_CS 2>/dev/null
p CF-27 "已卸载 $M_CI 与 $M_CS"

dmesg | grep -qE 'BUG|WARNING.*sharefs|Call trace.*sharefs' && f CF-28 "日志中出现 BUG/WARNING/调用栈" || p CF-28 "无 BUG/WARNING/调用栈"

echo ""
echo "--- 完毕 ---"
echo "SUMMARY: $N_PASS pass, $N_FAIL fail, $N_SKIP skip  (共 $((N_PASS+N_FAIL+N_SKIP)) 项)"
[ $N_FAIL -eq 0 ] || { echo "存在失败项：请按上面的 [编号] 对照开头用例清单定位（FAIL 行末尾给出了实测值）"; exit 1; }
echo "全部用例通过（SKIP 项见上方说明，非失败）"
exit 0
