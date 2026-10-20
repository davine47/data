# RISC-V branchstat：QEMU 动态分支比例参考插件

面向本地研究实验，核对接口的源码版本为 `/Users/wenjunnan/qemu` 中的
QEMU 11.1.50、plugin API 7。此目录未修改 QEMU 源码。

## 文件

- `branchstat.c`：完整插件。
- `riscv_branch_decode.h`：独立的标准 I+C 分支编码分类器。
- `test_branch_decode.c`：不依赖 QEMU 的解码测试。

## 统计定义

动态执行事件计数，而非静态指令数量或 taken rate：

```
branches = conditional + direct + indirect
conditional_ratio = 100 * conditional / branches
```

- 条件：BEQ、BNE、BLT、BGE、BLTU、BGEU、C.BEQZ、C.BNEZ。
- 直接：JAL、C.J、RV32 的 C.JAL。
- 间接：JALR、C.JR、C.JALR，包括使用这些编码的调用和返回。
- ECALL、EBREAK、MRET、SRET、中断入口不计为上述分支。
- `beq x0,x0,target` 仍按编码计为条件分支。
- 计数在指令执行前更新；异常时不保证等于已退休指令数。
- XLEN 由 `info->target_name` 的 riscv32/riscv64 决定，假定整个执行过程
  与该 XLEN 一致；不处理 RV64 系统中切换至 RV32 执行模式的情况。
- 仅覆盖标准 I+C 控制转移编码。Zcmp/Zcmt、自定义等额外控制转移扩展
  未覆盖；`unsupported_length=0` 不代表覆盖了全部已启用扩展。
- 不提供目标进程、地址范围或 ROI 过滤：system 模式包括固件、内核等。
- 每个 vCPU 独立计数；整体比例使用汇总计数计算。
- 未提供 taken/not-taken、逐区间曲线或 checkpoint 保存功能。

## 方式一：接入当前 QEMU 的 Meson（集成操作说明，未自动执行）

把 `branchstat.c` 与 `riscv_branch_decode.h` 一起复制到
QEMU 的 `contrib/plugins/` 目录。

在 `contrib/plugins/meson.build` 的 `contrib_plugins` 列表加入：

```meson
'branchstat.c',
```

若尚未配置，在 QEMU 根目录执行：

```sh
mkdir -p build-debug
cd build-debug
../configure --target-list=riscv64-softmmu --enable-plugins \
    --enable-debug --disable-strip
make -j8
```

若已有对应构建目录，新增插件后执行：

```sh
ninja -C build-debug contrib-plugins
```

插件通常生成于 `build-debug/contrib/plugins/libbranchstat.so`。
Meson 会按宿主平台选择模块后缀，应以实际生成文件为准。
Linux 上需要 user mode 时，配置目标可包含 `riscv64-linux-user`；
macOS 宿主不能直接构建 Linux user-mode 模拟器。

## 方式二：独立编译，不修改 QEMU 构建文件

需要宿主 C 编译器、GLib 开发文件、pkg-config 和匹配版本的 QEMU 头文件。
在本目录执行，头文件路径按实际环境替换。

Linux：

```sh
cc -std=c11 -O2 -g -fPIC -fvisibility=hidden -shared \
    -I/Users/wenjunnan/qemu/include/plugins \
    $(pkg-config --cflags glib-2.0) \
    branchstat.c -o libbranchstat.so \
    $(pkg-config --libs glib-2.0)
```

macOS：QEMU 插件 API 符号由加载它的 QEMU 宿主进程解析。

```sh
cc -std=c11 -O2 -g -fPIC -fvisibility=hidden \
    -bundle -Wl,-undefined,dynamic_lookup \
    -I/Users/wenjunnan/qemu/include/plugins \
    $(pkg-config --cflags glib-2.0) \
    branchstat.c -o libbranchstat.so \
    $(pkg-config --libs glib-2.0)
```

插件应使用宿主编译器和宿主架构，不能用 RISC-V 客户交叉编译器生成。

## 加载插件

Linux user mode（假设应用可直接由该模拟器运行）：

```sh
qemu-riscv64 -plugin ./libbranchstat.so,outfile=branchstats-run1.txt ./app
```

system mode：在原来可用的整套启动参数中加入：

```
-accel tcg -plugin /absolute/path/libbranchstat.so,outfile=/absolute/path/run1.txt
```

`outfile` 是唯一插件参数。文件使用独占创建，已经存在会报错，避免覆盖
以前的结果；换一个新文件名即可。不提供 `outfile` 或指定 `outfile=-`
时，通过 QEMU 插件日志输出，需启用 `-d plugin`，例如：

```sh
qemu-riscv64 -plugin ./libbranchstat.so -d plugin -D branchstats.log ./app
```

正常退出时才输出完整报告；强制终止进程可能得不到报告。QEMU VM 快照
不自动持久化插件 scoreboard，跨 checkpoint 的计数需另外设计。

## 验证

本次生成后的检查结果：131,143 项解码检查通过（启用 AddressSanitizer 和
UndefinedBehaviorSanitizer）；使用本地 QEMU 11.1.50/API 7 头文件和 GLib，
在 macOS 上以 `-Wall -Wextra -Werror` 编译插件成功，并确认两个必需导出符号
存在。当前环境没有可运行的 QEMU 可执行文件，尚未完成真实客户程序的运行
计数验证。上面的编译命令和下面的预期值需在你的 QEMU 运行环境继续验证。

本地解码测试：

```sh
cc -std=c11 -Wall -Wextra -Werror test_branch_decode.c -o test_branch_decode
./test_branch_decode
```

测试包括 RV32/RV64、六种条件分支、JAL/JALR、压缩调用/返回、
RV64 C.ADDIW 与 RV32 C.JAL 区分，以及全部 16 位编码组合的分类检查。
该分类器不是完整的非法指令检测器；需要 QEMU 对客户 ISA 作有效性检查。

建议进一步使用没有 CRT 启动代码、关闭 RVC 的简单客户汇编验证运行计数：
循环条件分支执行 100 次、结束后一次 JAL x0；不计用于退出的 ECALL，
预期条件数 100、直接跳转数 1、间接跳转数 0、比例约 99.009900990%。

CLion 调试 QEMU 时，Executable 填 QEMU，Program arguments 中加入
`-plugin ...`；可在 `qemu_plugin_install`、`on_tb_translate`、`plugin_exit`
打断点。inline 计数不经过每条指令的 C callback。

## QEMU 项目贡献范围

这是一份 AI 辅助生成的本地研究参考代码。当前 QEMU 的 `AGENTS.md` 和
`docs/devel/code-provenance.rst` 允许本地实验和 API 研究，但禁止将此类
生成内容作为上游贡献提交。
