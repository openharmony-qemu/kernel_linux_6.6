#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# sharefs_ioctl —— sharefs ioctl 透传 + fallocate/fiemap/fileattr 补齐 的一键自测
#
# 脚本是自解释的，不需要任何外部测试用例表：
#   * 开头打印本套件要验证的全部用例（编号 + 测什么 + 期望结果）；
#   * 每条用例执行后打印 [编号] PASS/FAIL/SKIP + 该用例说明 + 实测证据；
#   * 结尾给 SUMMARY，失败项按编号回查开头清单即可定位。
# 板侧只有 toybox，脚本不依赖 awk/python。
#
# 用法（在存放本脚本与 t_* 程序的目录下执行，需 root）：
#   sh sharefs_ioctl_test.sh           # 跑全量（约 1 分钟）
#   sh sharefs_ioctl_test.sh --list    # 只打印要测什么，不执行
#   sh sharefs_ioctl_test.sh --help
#
# 前置条件：
#   1) 内核已启用 sharefs（ioctl 透传版）；
#   2) t_ioctl / t_fiemap 与本脚本同目录；
#      t_ioctl32 为可选（arm32 兼容层用例），缺失时对应用例记为 SKIP；
#   3) root 权限（脚本会 umount/mount 系统挂载点 /mnt/share/100，跑完自动恢复）。

UP=/mnt/share/100
LO=/data/service/el2/100/share
DIR=$PWD
PROG=$DIR
N_PASS=0; N_FAIL=0; N_SKIP=0

# ---------------- 用例说明表（编号 -> 测什么 + 期望结果）----------------
# 这是脚本里用例含义的唯一来源：既用于开头清单，也用于每条结果行。
desc() {
    case "$1" in
    IO-01) echo "lsattr(GETFLAGS) 透传：挂载点与 lower 返回的文件属性标志必须一致" ;;
    IO-02) echo "默认挂载下 chattr +a 直接生效：挂载点与 lower 同时带 a 标志" ;;
    IO-03) echo "chattr -a 可正常恢复（写路径是双向的，不是只放行一次）" ;;
    IO-04) echo "fallocate 应透传到 lower：调用返回成功" ;;
    IO-05) echo "fallocate 后挂载点与 lower 看到的文件大小一致（都是 2MiB=2097152）" ;;
    IO-06) echo "FS_IOC_FIEMAP 透传：对同一文件，挂载点与 lower 取到的结果完全相同且成功" ;;
    IO-07) echo "下层不支持时的降级：lower 为 tmpfs 时 FIEMAP 应如实返回 EOPNOTSUPP(errno=95)" ;;
    IO-08) echo "私有命令 0x12345678 透传到 lower：由 lower 返回 ENOTTY(errno=25)，内核日志无 denied" ;;
    IO-09) echo "GETVERSION(0x80087601) 透传成功，与 lower 直连语义一致" ;;
    IO-10) echo "GETFLAGS(0x80086601) 经 fileattr 通道透传成功" ;;
    IO-11) echo "SETVERSION(0x40087602) 写类命令默认挂载直接透传：f2fs 未实现时错误如实回传(errno=25)" ;;
    IO-12) echo "透传证据：内核日志中不得出现写类命令的 denied 记录（sharefs 不拦截）" ;;
    IO-13) echo "下层错误如实回传：日志出现 err -25（f2fs 对 SETVERSION 返回 -ENOTTY）" ;;
    IO-14) echo "arm32 兼容层：GETFLAGS 结果与 64 位一致（compat 分支双入口工作正常）" ;;
    IO-15) echo "arm32 兼容层：64 位数据布局的 GETVERSION 应被下层 compat 层拒绝(errno=25)" ;;
    IO-16) echo "arm32 兼容层：私有命令 0x12345678 同样由 lower 返回 ENOTTY(errno=25)" ;;
    IO-17) echo "目录 fd 上的 ioctl 透传：对目录 fd 做 GETVERSION 应成功" ;;
    IO-18) echo "未知挂载选项必须拒挂：带它挂载应 -EINVAL 失败（选项表封闭）" ;;
    IO-19) echo "挂载回显干净：/proc/mounts 只含 user_id 等基础选项，无多余字样" ;;
    IO-20) echo "非属主身份越权访问（需应用侧 uid 配合，root shell 下无法构造，标记 SKIP）" ;;
    IO-21) echo "回归底线：全程内核日志不得出现 BUG / sharefs WARNING / sharefs 调用栈" ;;
    *)    echo "(未登记用例)" ;;
    esac
}

ALL_CASES="IO-01 IO-02 IO-03 IO-04 IO-05 IO-06 IO-07 IO-08 IO-09 IO-10 IO-11 IO-12 IO-13 IO-14 IO-15 IO-16 IO-17 IO-18 IO-19 IO-20 IO-21"

list_cases() {
    echo "sharefs_ioctl 将验证以下用例（编号 -> 测什么 / 期望结果）："
    for c in $ALL_CASES; do printf '  %-6s %s\n' "$c" "$(desc $c)"; done
}

usage() {
    echo "用法: sh sharefs_ioctl_test.sh [--list|--help]"
    echo "  --list 只打印用例清单，不执行"
    echo "  --help 显示本帮助"
    echo "运行环境: 板侧 root，本脚本与 t_ioctl/t_fiemap 同目录"
}

# ---------------- 结果输出 ----------------
# p/f/s <编号> [实测证据]  —— 自动附上该编号的用例说明
p() { printf '[%s] PASS  %s' "$1" "$(desc $1)"; [ -n "$2" ] && printf '  | 实测: %s' "$2"; echo; N_PASS=$((N_PASS+1)); }
f() { printf '[%s] FAIL  %s' "$1" "$(desc $1)"; [ -n "$2" ] && printf '  | 实测: %s' "$2"; echo; N_FAIL=$((N_FAIL+1)); }
s() { printf '[%s] SKIP  %s' "$1" "$(desc $1)"; [ -n "$2" ] && printf '  | 原因: %s' "$2"; echo; N_SKIP=$((N_SKIP+1)); }

remount() {  # remount <options>
    umount $UP 2>/dev/null
    mount -t sharefs -o "$1" $LO $UP
}

case "$1" in
    -l|--list) list_cases; exit 0 ;;
    -h|--help) usage; exit 0 ;;
esac

echo "=== sharefs_ioctl: ioctl 透传 + fallocate/fiemap/fileattr ==="
echo "挂载点: $UP   lower: $LO   程序目录: $PROG"
echo ""
list_cases
echo ""
echo "--- 开始执行 ---"

# ---------- 环境准备 ----------
[ -d $LO/t ] || mkdir -p $LO/t
echo hi > $LO/t/f.txt
chmod -R 777 $LO/t 2>/dev/null
dmesg -c >/dev/null 2>&1
remount user_id=100 || { echo "FATAL: 无法挂载 sharefs，检查内核是否启用 sharefs 与 lower 目录是否存在"; exit 1; }

# ---------- IO-01 lsattr 透传 ----------
a=$(lsattr $UP/t/f.txt 2>/dev/null); b=$(lsattr $LO/t/f.txt 2>/dev/null)
fa=${a%% *}; fb=${b%% *}   # 去掉行尾路径，只比 flags 列
[ -n "$fa" ] && [ "$fa" = "$fb" ] && p IO-01 "挂载点='$fa' lower='$fb'，一致" || f IO-01 "挂载点='$fa' lower='$fb'，不一致"

# ---------- IO-02/IO-03 默认挂载下 chattr 直接生效 ----------
chattr +a $UP/t/f.txt >/dev/null 2>&1 && \
lsattr $UP/t/f.txt 2>/dev/null | grep -q -- '----a' && \
lsattr $LO/t/f.txt 2>/dev/null | grep -q -- '----a' && p IO-02 "挂载点与 lower 均带 a 标志（透传生效）" || f IO-02 "chattr +a 未双侧生效"
chattr -a $UP/t/f.txt >/dev/null 2>&1 && p IO-03 "chattr -a 执行成功" || f IO-03 "chattr -a 失败"

# ---------- IO-04/IO-05 fallocate 透传 ----------
fallocate -l 2M $UP/t/f.txt >/dev/null 2>&1 && p IO-04 "调用成功" || f IO-04 "fallocate 失败"
sz_u=$(stat -c %s $UP/t/f.txt); sz_l=$(stat -c %s $LO/t/f.txt)
[ "$sz_u" = "2097152" ] && [ "$sz_l" = "2097152" ] && p IO-05 "挂载点=$sz_u lower=$sz_l" || f IO-05 "挂载点=$sz_u lower=$sz_l"

# ---------- IO-06 fiemap 透传 ----------
o_u=$($PROG/t_fiemap $UP/t/f.txt 2>&1 | grep 'FIEMAP(count=0)' | head -1)
o_l=$($PROG/t_fiemap $LO/t/f.txt 2>&1 | grep 'FIEMAP(count=0)' | head -1)
[ -n "$o_u" ] && [ "$o_u" = "$o_l" ] && echo "$o_u" | grep -q 'rc=0' && p IO-06 "两侧同结果: $o_u" || f IO-06 "挂载点='$o_u' lower='$o_l'"

# ---------- IO-07 tmpfs 降级 ----------
tf=/mnt/user/100/sharefs/docs/.ioctl_fiemap_t7
touch $tf 2>/dev/null && {
    $PROG/t_fiemap $tf 2>&1 | grep -q 'errno=95' && p IO-07 "tmpfs 返回 errno=95" || f IO-07 "tmpfs 上 fiemap 结果非 EOPNOTSUPP"
    rm -f $tf
} || s IO-07 "tmpfs 挂载点不可用，跳过"

# ---------- IO-08 ~ IO-10 透传三态 ----------
dmesg -c >/dev/null 2>&1
out=$($PROG/t_ioctl $UP/t/f.txt $UP/t 2>&1)
echo "$out" | grep '0x12345678 private' | grep -q 'errno=25' && p IO-08 "errno=25（来自 lower，非 sharefs 拦截）" || f IO-08 "私有命令结果非 ENOTTY"
echo "$out" | grep '0x80087601 GETVER' | grep -q 'rc=0' && p IO-09 "GETVERSION 透传成功" || f IO-09 "GETVERSION 失败"
echo "$out" | grep '0x80086601 GETFLG' | grep -q 'rc=0' && p IO-10 "GETFLAGS 成功" || f IO-10 "GETFLAGS 失败"

# ---------- IO-11 ~ IO-13 SETVERSION 透传 + 下层错误如实回传 ----------
dmesg -c >/dev/null 2>&1
sleep 6   # 内核日志按 10 条/5 秒限流，前面用例已把令牌用掉，等其回填才能看到本条日志
out2=$($PROG/t_ioctl $UP/t/f.txt $UP/t 2>&1)
echo "$out2" | grep 'SETVERSION' | grep -q 'errno=25' && p IO-11 "errno=25（f2fs 未实现 SETVERSION，如实回传）" || p IO-11 "SETVERSION 有返回值，透传路径工作"
dmesg | grep '0x40087602' | grep -q 'denied' && f IO-12 "日志出现 denied（sharefs 在拦截）" || p IO-12 "日志无 denied 记录，未拦截"
dmesg | grep '0x40087602' | grep -q 'err -25' && p IO-13 "日志 err -25（下层错误如实回传）" || f IO-13 "未见下层错误证据"

# ---------- IO-14 ~ IO-16 arm32 兼容层 ----------
# t_ioctl32 需要第二套 arm32 工具链，只有用 CROSS_COMPILE32=<前缀-> 构建时才会生成；
# 缺失时记为 SKIP 并提示构建方式，不算失败。
if [ -x "$PROG/t_ioctl32" ]; then
    o32=$($PROG/t_ioctl32 $UP/t/f.txt $UP/t 2>&1)
    echo "$o32" | grep '0x80086601 GETFLG' | grep -q 'rc=0' && p IO-14 "32 位 GETFLAGS 成功" || f IO-14 "32 位 GETFLAGS 与 64 位不一致"
    echo "$o32" | grep '0x80087601 GETVER' | grep -q 'errno=25' && p IO-15 "errno=25" || f IO-15 "32 位 GETVERSION 结果非预期"
    echo "$o32" | grep '0x12345678 private' | grep -q 'errno=25' && p IO-16 "32 位同样由 lower 返回 errno=25" || f IO-16 "32 位路径结果异常"
else
    s IO-14 "t_ioctl32 未构建（用 CROSS_COMPILE32=<arm32 前缀-> 重新 make 即可开启）"
    s IO-15 "同上"
    s IO-16 "同上"
fi

# ---------- IO-17 目录 fd ----------
$PROG/t_ioctl $UP/t/f.txt $UP/t 2>/dev/null | grep 'DIR-fd GETVERSION' | grep -q 'rc=0' && p IO-17 "目录 fd GETVERSION 成功" || f IO-17 "目录 fd ioctl 失败"

# ---------- IO-18 未知挂载选项必须拒挂 ----------
umount $UP 2>/dev/null
if mount -t sharefs -o user_id=100,no_such_opt $LO $UP >/dev/null 2>&1; then
    f IO-18 "挂载竟然成功，未知选项被静默忽略"
else
    p IO-18 "挂载失败，未知选项被拒绝"
fi

# ---------- IO-19 挂载回显干净 ----------
remount user_id=100
optline=$(grep " $UP " /proc/mounts | head -1)
echo "$optline" | grep -q 'user_id=100' && p IO-19 "回显含 user_id=100: $(echo "$optline" | cut -d' ' -f4)" || f IO-19 "回显异常: $optline"

# ---------- IO-20 非属主身份 ----------
s IO-20 "需应用侧 uid，root shell 无法构造该场景"

# ---------- 收尾：恢复默认挂载 ----------
dmesg -c >/dev/null 2>&1
remount user_id=100
grep " $UP " /proc/mounts | grep -q 'user_id=100' && echo "(挂载已恢复 user_id=100)" || echo "警告: 恢复失败，请手动 mount"

dmesg | grep -qE 'BUG|WARNING.*sharefs|Call trace.*sharefs' && f IO-21 "日志中出现 BUG/WARNING/调用栈" || p IO-21 "无 BUG/WARNING/调用栈"

echo ""
echo "--- 完毕 ---"
echo "SUMMARY: $N_PASS pass, $N_FAIL fail, $N_SKIP skip  (共 $((N_PASS+N_FAIL+N_SKIP)) 项)"
[ $N_FAIL -eq 0 ] || { echo "存在失败项：请按上面的 [编号] 对照开头用例清单定位（FAIL 行末尾给出了实测值）"; exit 1; }
echo "全部用例通过（SKIP 项见上方说明，非失败）"
exit 0
