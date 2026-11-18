# xv6_learn
for learning xv6

基于 MIT 6.1810 Fall 2024 的 xv6 RISC-V 实验仓库。各实验按 Git 分支组织，可以切换到对应分支查看实现和运行测试。

## 实验索引

| 分支 | 实验内容 |
| --- | --- |
| `syscall` | 实现 `trace` 和 `sysinfo` 系统调用。 |
| `pgtbl` | 添加 `USYSCALL` 用户只读映射和 2 MiB 大页支持。 |
| `traps` | 实现周期性用户态闹钟 `sigalarm` 和上下文恢复 `sigreturn`。 |
| `cow` | 实现写时复制 `fork` 和物理页引用计数。此分支也收录了 `sleep`、`pingpong`、`primes`、`find`、`xargs`。 |
| `lock` | 将物理页分配器改为每 CPU 空闲链表，空闲页不足时从其他 CPU 获取，减少锁竞争。 |
| `fs` | 通过二级间接块支持更大的文件，并实现符号链接。 |
| `mmap` | 实现文件映射 `mmap` 和解除映射 `munmap`。 |

## 构建与测试

需要 GNU Make、RISC-V 交叉编译工具链和 `qemu-system-riscv64`。以 `lock` 分支为例，在仓库目录执行：

```sh
git switch lock
make clean
make qemu
```

在 QEMU 中按 `Ctrl-a`，松开后按 `x` 退出。在宿主机运行当前分支的评分脚本：

```sh
make grade
```

## 上游资料

- [MIT 6.1810 Fall 2024 课程资料](https://pdos.csail.mit.edu/6.1810/2024/)
- [原版 xv6 README 和致谢](README)
- [许可证](LICENSE)
