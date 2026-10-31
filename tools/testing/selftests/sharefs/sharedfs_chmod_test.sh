#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (c) 2026 Huawei Device Co., Ltd.

# sharefs chmod control selftest ("reverse umask", RM.001).
# Semantics finalized per the requirement alignment with Huawei:
#   - share_perm_mask is a compile-time fixed default of 0660, not a
#     mount option
#   - pure OR on the u/g r/w bits (bits set in the mask cannot be cleared
#     by chmod)
#   - x and other-class bits follow the request (directories may be
#     locked down by chmod)
#   - suid/sgid/sticky bits are dropped
#
# Emits TAP13; run as root on the rk3568 board (hdc shell).
#
# Usage: sh sharedfs_chmod_test.sh
# Environment:
#   SKIP_SETUP=1  lower dir + sharefs already mounted (plain mount with
#                 -o user_id=0), skip setup
#   KEEP_ENV=1    do not unmount/clean up on exit (debugging)
#   DATADIR       working directory, default /data
#   FSOP          chmod_fsop tool path, default /data/local/tmp/chmod_fsop
#
# chmod_fsop build instructions: see the header of chmod_fsop.c.

DATADIR=${DATADIR:-/data}
LOWER=$DATADIR/sharefs_lower
MNT=$DATADIR/sharefs_mnt
FSOP=${FSOP:-/data/local/tmp/chmod_fsop}

n=0
pass=0
fail=0

plan()
{
	echo "1..15"
}

ok()
{
	n=$((n + 1)); pass=$((pass + 1))
	echo "ok $n - $1"
}

not_ok()
{
	n=$((n + 1)); fail=$((fail + 1))
	echo "not ok $n - $1 (want '$2', got '$3')"
}

check_eq()
{
	if [ "$2" = "$3" ]; then
		ok "$1"
	else
		not_ok "$1" "$2" "$3"
	fi
}

# Read permission bits via chmod_fsop statmode (octal, no leading zero)
mode_of()
{
	"$FSOP" 0 0 statmode "$1" 2>/dev/null
}

cleanup()
{
	if [ "$KEEP_ENV" = "1" ]; then
		echo "# KEEP_ENV=1, env left at $LOWER / $MNT"
		return
	fi
	umount "$MNT" 2>/dev/null
}

# ---------- setup ----------
# RM.001 needs no loop/ext4 image: a plain directory on /data works as the
# lower layer. (Only the case-insensitive tests need a casefold-enabled
# lower; the board has no losetup, so use a host-prepared partition then.)
[ "$SKIP_SETUP" = "1" ] || {
	# Defensive cleanup: if a previous ioctl-suite run set FS_IMMUTABLE on
	# f1 and its clear-flag case failed, leaving the bit set would make
	# every later rm/touch/chmod fail with EPERM.
	[ -x "$FSOP" ] && "$FSOP" 0 0 setflags "$LOWER/f1" 0 >/dev/null 2>&1

	rm -rf "$LOWER" "$MNT"
	mkdir -p "$LOWER" "$MNT"

	touch "$LOWER/f1"
	mkdir "$LOWER/d1"

	mount -t sharefs -o user_id=0 "$LOWER" "$MNT" || {
		echo "BAIL: sharefs mount failed (is CONFIG_SHARE_FS=y?)"; exit 1; }
}

trap cleanup EXIT
plan

# ---------- cases (mask fixed at 0660, pure OR; expectations always read
# the lower file) ----------
# Why the lower file: the real permission verdict in sharefs_permission
# reads the lower ext4 mode (the enforcement surface); the upper display
# mode is re-stamped on every lookup by the pre-existing sharefs
# authorization display model (fixup_perm_from_level forces 0550 on
# children of the mount root) -- that is existing sharefs design,
# unrelated to RM.001. The upper/lower convention was finalized in the
# design doc (requirements design-4) and the acceptance notes.

# 1 core: after chmod 000, the mask-locked r/w bits are restored
chmod 000 "$MNT/f1" 2>/dev/null
check_eq "chmod_000_lower_660" "660" "$(mode_of "$LOWER/f1")"

# 2 x bits follow the request
chmod 111 "$MNT/f1" 2>/dev/null
check_eq "chmod_111_x_follows_request" "771" "$(mode_of "$LOWER/f1")"

# 3 other-class bits follow the request
chmod 004 "$MNT/f1" 2>/dev/null
check_eq "chmod_004_other_follows_request" "664" "$(mode_of "$LOWER/f1")"

# 4 pure OR: only u=w requested, g=r is also restored from the mask
# (200|660=660)
chmod 200 "$MNT/f1" 2>/dev/null
check_eq "chmod_200_pure_or_group_r" "660" "$(mode_of "$LOWER/f1")"

# 5 pure OR: g=w is restored (644|660=664)
chmod 644 "$MNT/f1" 2>/dev/null
check_eq "chmod_644_group_w_pulled" "664" "$(mode_of "$LOWER/f1")"

# 6 pure OR: 0555 -> 775 (u/g r|w completed from the mask, x/other-class
# bits follow the request)
chmod 0555 "$MNT/f1" 2>/dev/null
check_eq "chmod_0555_pure_or_775" "775" "$(mode_of "$LOWER/f1")"

# 7 directory: x bits follow the request; directories may be locked down
# (per Q2)
chmod 000 "$MNT/d1" 2>/dev/null
check_eq "dir_chmod_000_lower_660" "660" "$(mode_of "$LOWER/d1")"

# 8 directory: g=w is restored (755|660=775)
chmod 755 "$MNT/d1" 2>/dev/null
check_eq "dir_chmod_755_lower_775" "775" "$(mode_of "$LOWER/d1")"

# 9 suid/sgid/sticky bits are dropped (per Q3)
chmod 4755 "$MNT/f1" 2>/dev/null
check_eq "suid_bit_dropped_4755_lower" "775" "$(mode_of "$LOWER/f1")"

# 10 core regression after the full round
chmod 000 "$MNT/f1" 2>/dev/null
check_eq "chmod_000_lower_660_final" "660" "$(mode_of "$LOWER/f1")"

# 11 lower layer keeps the enforced mode (660)
check_eq "lower_real_660_final" "660" "$(mode_of "$LOWER/f1")"

# 12 chown is silently ineffective (ATTR_UID/GID stripped by design)
chown 2000010 "$MNT/f1" 2>/dev/null
check_eq "chown_exit_0" "0" "$?"
check_eq "chown_uid_unchanged" "0" "$("$FSOP" 0 0 statuid "$MNT/f1")"

# 13 truncate (ATTR_SIZE) passes through unchanged
echo hello > "$MNT/f1" 2>/dev/null
"$FSOP" 0 0 truncate "$MNT/f1" 100 >/dev/null 2>&1
check_eq "truncate_exit_0" "0" "$?"
size=$(wc -c < "$MNT/f1" 2>/dev/null)
check_eq "truncate_size_applied" "100" "$((size + 0))"

# ---------- summary ----------
echo "# pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
