# sharefs selftests

针对 OpenHarmony `fs/sharefs` 两个需求的自测套件。两套测试彼此独立，可分别编译、分别执行：

| 目录中的脚本 | 主题 | 覆盖内容 |
|---|---|---|
| `sharefs_ioctl_test.sh` | ioctl 透传与文件属性补齐 | ioctl 纯透传语义、fallocate、fiemap、fileattr（chattr/lsattr）、未知挂载选项拒挂、arm32 兼容层 |
| `sharefs_casefold_test.sh` | ASCII 大小写不敏感适配 | 变体查找、精确名优先、折叠范围（非 ASCII / 数字）、只改大小写的 rename、rename 回归 |

脚本自带环境准备与现场恢复，跑完会打印**测了什么 + 是否成功**，不需要任何外部测试用例表。

---

## 1. 测试程序清单

| 程序 | 套件 | 验证什么 | 用法 |
|---|---|---|---|
| `t_ioctl` | sharefs_ioctl | ioctl 透传探针：私有命令(0x12345678)由 lower 返回 ENOTTY、GETVERSION/GETFLAGS/SETVERSION 透传、目录 fd ioctl | `t_ioctl <文件> <目录>` |
| `t_ioctl32` | sharefs_ioctl | 32 位兼容层走同一透传路径，且 64 位数据布局的命令被下层 compat 层正确拒绝 | `t_ioctl32 <文件> <目录>` |
| `t_fiemap` | sharefs_ioctl | FS_IOC_FIEMAP 透传，以及下层不支持时降级返回 EOPNOTSUPP | `t_fiemap <文件>` |
| `t_casefold_rename` | sharefs_casefold | 只改大小写的 rename 六分支矩阵（direct / EEXIST / 标准路径等） | `t_casefold_rename <大小写挂载点> <敏感挂载点>` |
| `t_ci_bench` | sharefs_casefold | 变体查找性能基线（精确命中、含字母 miss、纯数字 miss 各 200 次） | `t_ci_bench <大小写挂载点内目录>` |
| `t_rename_flags0` | sharefs_casefold | flags=0 的 rename 回归（目录 + 文件） | `t_rename_flags0 <大小写挂载点内目录>` |
| `t_rename_exchange` | sharefs_casefold | RENAME_EXCHANGE 应被拒（-EINVAL）且两文件内容不被改动 | `t_rename_exchange <大小写挂载点内目录>` |

每个程序启动时都会先打印"本程序验证什么 / 期望结果 / 怎么判"，单独跑也能看懂。

---

## 2. 编译

```bash
cd kernel/linux/linux-6.6/tools/testing/selftests

# 主程序（交叉编译；工具链由 lib.mk 从 CROSS_COMPILE 推导，Makefile 里没有私有工具链变量）
make ARCH=arm64 TARGETS=sharefs \
     CROSS_COMPILE=$HOME/code/oh/master/prebuilts/gcc/linux-x86/aarch64/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-

# 产物在 selftests/sharefs/ 下
```

与内核一起参与编译的开关就是 `selftests/Makefile` 里注册的一行 `TARGETS += sharefs`：

- 从内核根目录 `make ARCH=arm64 CROSS_COMPILE=<prefix-> kselftest-all` 会带上本套件；
- `SKIP_TARGETS="bpf sharefs"` 会把它排除；
- 标准 `make ... install INSTALL_PATH=<dir>` 会安装二进制、脚本与本文档，并生成 `run_kselftest.sh`。

**可选：32 位兼容层探针 `t_ioctl32`**

它需要第二套 arm32 工具链，做成开关式（工具链支持就编、不支持就跳过，与 x86 selftests 的做法一致）：

```bash
make ARCH=arm64 TARGETS=sharefs \
     CROSS_COMPILE=<aarch64-prefix-> \
     CROSS_COMPILE32=$HOME/code/oh/master/prebuilts/gcc/linux-x86/arm/gcc-linaro-7.5.0-arm-linux-gnueabi/bin/arm-linux-gnueabi-
```

不传 `CROSS_COMPILE32` 时不会生成 `t_ioctl32`，脚本把对应的 3 项记为 **SKIP** 并提示开启方式，不会误报失败。

---

## 3. 部署与运行

```bash
# PC 侧推送（hdc）
for f in t_ioctl t_ioctl32 t_fiemap t_casefold_rename t_ci_bench \
         t_rename_exchange t_rename_flags0 sharefs_ioctl_test.sh sharefs_casefold_test.sh; do
    hdc file send $f /data/local/tmp/kselftest_sharefs/
done

# 板侧（hdc shell，root）
cd /data/local/tmp/kselftest_sharefs
sh sharefs_ioctl_test.sh       # ioctl 套件全量（约 1 分钟）
sh sharefs_casefold_test.sh    # 大小写套件全量（约 1 分钟）
```

只看要测什么、不执行：

```bash
sh sharefs_ioctl_test.sh --list
sh sharefs_casefold_test.sh --list
```

两个脚本都只依赖 toybox（sh/grep/sed），不需要 python/awk。

运行前置条件：

- sharefs_ioctl：内核启用 sharefs，lower 目录 `/data/service/el2/100/share` 可访问，root 权限
  （脚本会 umount/mount 系统挂载点 `/mnt/share/100`，跑完自动恢复默认挂载）。
- sharefs_casefold：内核启用 sharefs 的 casefold 能力（挂载选项 `case_sensitive` 可用），root 权限
  （脚本在 `/data/local/tmp` 下自建 lower 目录与两个挂载点，跑完卸载）。
- **只改大小写的 rename 分支**由 `fs/sharefs/Makefile` 里的
  `ccflags-y += -DSHAREFS_CASEFOLD_RENAME_EXPERIMENTAL` 控制，默认注释掉。
  需要覆盖该分支的用例（CF-11 / CF-13 / CF-20 / CF-21）必须先打开它重新编译内核，
  否则这几项会以"日志无 direct 分支证据"失败。

---

## 4. 输出怎么读

脚本先打印本套件的完整用例清单，再逐条执行：

```
[IO-08] PASS  私有命令 0x12345678 透传到 lower  | 实测: errno=25（来自 lower，非 sharefs 拦截）
[IO-09] FAIL  GETVERSION 透传成功  | 实测: GETVERSION 失败
```

- `[编号]` 用于定位；后面一句是该用例"测什么 + 期望结果"；`| 实测:` 后面是实际观测到的值。
- `PASS` / `FAIL` / `SKIP` 三种结果；`SKIP` 会带上原因（例如缺少 `t_ioctl32`、场景无法在 root shell 构造）。
- 结尾 `SUMMARY: N pass, M fail, K skip`——`M == 0` 即通过；非 0 时按 `[编号]` 回查开头清单，
  或跑 `--list` 再看一遍。

### sharefs_ioctl 用例（IO-01 ~ IO-21）

| 编号 | 测什么 / 期望结果 |
|---|---|
| IO-01 | lsattr(GETFLAGS) 透传：挂载点与 lower 属性标志一致 |
| IO-02 | 默认挂载下 chattr +a 直接生效（挂载点与 lower 双侧） |
| IO-03 | chattr -a 可恢复 |
| IO-04 | fallocate 透传成功 |
| IO-05 | fallocate 后双向大小一致（2MiB） |
| IO-06 | FIEMAP 透传：挂载点与 lower 结果一致 |
| IO-07 | 下层为 tmpfs 时 FIEMAP 降级返回 EOPNOTSUPP(95) |
| IO-08 | 私有命令 0x12345678 由 lower 返回 ENOTTY(25)，日志无 denied |
| IO-09 | GETVERSION(0x80087601) 透传成功 |
| IO-10 | GETFLAGS(0x80086601) 经 fileattr 通道成功 |
| IO-11 | SETVERSION(0x40087602) 默认挂载直接透传（f2fs 未实现则 errno=25 如实回传） |
| IO-12 | 透传证据：日志无该写类命令 denied 记录（sharefs 不拦截） |
| IO-13 | 下层错误如实回传（日志 err -25） |
| IO-14 | arm32 兼容层 GETFLAGS 与 64 位一致 |
| IO-15 | arm32 兼容层拒绝 64 位布局的 GETVERSION |
| IO-16 | arm32 兼容层私有命令同样由 lower 返回 ENOTTY |
| IO-17 | 目录 fd 上的 ioctl 透传成功 |
| IO-18 | 未知挂载选项拒挂（-EINVAL） |
| IO-19 | /proc/mounts 挂载回显正常（user_id=100） |
| IO-20 | 非属主身份越权（需应用侧 uid，SKIP） |
| IO-21 | 全程无 BUG / sharefs WARNING / 调用栈 |

### sharefs_casefold 用例（CF-01 ~ CF-28）

| 编号 | 测什么 / 期望结果 |
|---|---|
| CF-01 | 变体名 A.TXT 能命中已存在的某个大小写变体 |
| CF-02 | 日志出现 ci lookup hit（确实用了兜底路径） |
| CF-03 | a.txt 与 A.txt 是两个 inode，各自内容正确（精确名优先） |
| CF-04 | 变体扫描：A.TxT 命中已有变体 |
| CF-05 | 中文词干精确 + ASCII 扩展名折叠：中文文件.TXT 命中中文文件.txt |
| CF-06 | 中文精确名可访问 |
| CF-07 | 纯数字名不参与折叠，按精确名访问 |
| CF-08 | case_sensitive 挂载上变体名必须 ENOENT |
| CF-09 | case_sensitive 挂载上精确名可访问 |
| CF-10 | 只改大小写的 rename 调用成功 |
| CF-11 | 改名结果落到 lower 盘面（readdir 见到新拼写） |
| CF-12 | 改完后旧变体仍能读到原内容 |
| CF-13 | 日志出现 casefold rename (direct), err 0 |
| CF-14 | 同名 rename 是无副作用 no-op |
| CF-15 | 同名 rename 后内容完好 |
| CF-16 | 同目录普通改名落到 lower |
| CF-17 | 跨目录改名落到 lower 目标目录 |
| CF-18 | 路径逐段生效：SUB/S.TXT 命中 sub/s.txt |
| CF-19 | 用变体名删除生效 |
| CF-20 | 只改大小写的 rename 六分支矩阵全部通过 |
| CF-21 | 日志有 casefold rename (direct) |
| CF-22 | 日志有 casefold rename target exists（EEXIST 分支） |
| CF-23 | 敏感挂载走标准路径，不进 casefold 分支 |
| CF-24 | flags=0 的 rename 回归（目录 + 文件） |
| CF-25 | RENAME_EXCHANGE 被拒且内容不变 |
| CF-26 | 变体查找性能基线可跑通 |
| CF-27 | 收尾卸载自建挂载点 |
| CF-28 | 全程无 BUG / sharefs WARNING / 调用栈 |

---

## 5. 判定口径与已知边界

- **盘面拼写用 readdir 判定，不用 stat**：只改大小写的 rename 在探测阶段会给新名字留下一条
  lower 侧的 negative dentry，导致直接 stat lower 路径短暂返回 ENOENT（盘上其实已经改名成功，
  日志为 `casefold rename (direct), err 0`）。经大小写挂载访问一次即自愈。
  因此脚本一律以 readdir + 挂载点语义 + 内核日志三方证据为准。
- **toybox mv 不能直接用来验证只改大小写的 rename**：它会先 stat 目标名，在大小写挂载下
  目标名与源名解析到同一 inode，会被当成 same-file 直接 no-op。相关用例改用 `renameat2(flags=0)`
  的探针程序（CF-24）。
- **内核日志有限流**（默认 10 条 / 5 秒）：脚本在需要读日志的用例前会清空 dmesg 并等待令牌回填，
  否则会漏掉证据行。
- **arm32 兼容层用例**依赖 `t_ioctl32`，未构建时记为 SKIP 而非 FAIL。
非属主身份（IO-20）**需要应用侧 uid 配合，root shell 下无法构造，恒为 SKIP。
- **CI-native lower（如 vfat）上的 two-step 分支**在当前板（f2fs lower）不会触发，属预期。
