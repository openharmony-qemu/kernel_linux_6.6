@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem
rem Copyright (c) 2026 Huawei Device Co., Ltd.
rem push and run sharefs RM.001 selftest (15 TAP cases)
rem put this bat in the same folder as chmod_fsop and sharedfs_chmod_test.sh
hdc file send chmod_fsop /data/local/tmp/chmod_fsop || goto :err
hdc shell chmod +x /data/local/tmp/chmod_fsop || goto :err
hdc file send sharedfs_chmod_test.sh /data/local/tmp/ || goto :err
hdc shell sh /data/local/tmp/sharedfs_chmod_test.sh
pause
exit /b 0
:err
echo PUSH FAILED - check hdc connection (hdc list targets)
pause
exit /b 1
