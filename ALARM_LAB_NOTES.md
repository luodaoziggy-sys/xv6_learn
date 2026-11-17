# xv6 Alarm 实验实现总结

本文总结 `alarmtest` 的设计目标、实现思路、函数调用链、参数来源，以及用户态和内核态声明不同的原因。最后给出在 xv6 中添加一个系统调用的通用步骤。

## 一、Alarm 要实现什么

用户程序通过：

```c
sigalarm(interval, handler);
```

告诉内核：当前进程每消耗 `interval` 个 CPU 时钟周期，就暂停当前用户代码，转而执行一次用户态函数 `handler`。

处理函数结束时必须调用：

```c
sigreturn();
```

内核随后恢复闹钟发生前的所有用户寄存器，让程序像没有被打断一样从原位置继续执行。

完整过程是：

```text
用户程序调用 sigalarm(interval, handler)
              |
              v
内核记录周期和 handler 地址
              |
              v
每次 timer interrupt 增加计数
              |
              v
计数达到 interval
              |
              v
保存当前 trapframe，把 epc 改成 handler
              |
              v
返回用户态后从 handler 开始执行
              |
              v
handler 调用 sigreturn()
              |
              v
恢复原 trapframe，从被中断位置继续执行
```

## 二、为什么需要这些参数

用户态接口为：

```c
int sigalarm(int interval, void (*handler)());
```

### `interval`

`interval` 表示触发一次 alarm 需要经过多少个属于该进程的 timer tick。

例如：

```c
sigalarm(2, periodic);
```

表示进程每运行两个时钟 tick，就调用一次 `periodic()`。

当调用：

```c
sigalarm(0, 0);
```

`interval == 0` 表示关闭 alarm。因为定时器处理逻辑只在 `alarm_interval > 0` 时计数，所以 handler 地址为 `0` 时也不会被执行。

### `handler`

`handler` 是一个用户态函数指针：

```c
void (*handler)()
```

它保存函数第一条指令的用户虚拟地址。例如：

```c
void periodic(void)
{
  printf("alarm!\n");
  sigreturn();
}

sigalarm(2, periodic);
```

这里 `periodic` 不带括号，表示函数地址；`periodic()` 才表示立即调用函数。

内核不直接调用这个 C 函数。内核把用户 trapframe 中的 `epc` 改成该地址，等 `sret` 返回用户态后，由 CPU 从 handler 地址开始执行。

### 为什么还需要 `sigreturn()`

普通 C 函数通过 `ret` 返回，返回地址保存在 `ra` 中。但 alarm handler 不是由用户代码使用 `call` 调用的，而是内核直接修改 `epc` 跳转过去的，因此不存在一个正常的调用栈返回关系。

此外，handler 运行时会修改 `a0`、`a1`、`ra`、临时寄存器等状态。仅执行 `ret` 无法恢复被 timer interrupt 打断时的全部寄存器。

所以 handler 必须调用 `sigreturn()`，由内核恢复完整 trapframe。

## 三、进程需要保存哪些状态

在 `struct proc` 中增加：

```c
int alarm_interval;
uint64 alarm_handle;
int alarm_tick;
struct trapframe *alarm_trapframe;
int alarm_active;
```

每个字段的意义如下：

| 字段 | 作用 |
| --- | --- |
| `alarm_interval` | 用户设置的触发周期，`0` 表示关闭 |
| `alarm_handle` | 用户态 handler 的虚拟地址 |
| `alarm_tick` | 当前已经累计的时钟 tick 数 |
| `alarm_trapframe` | 保存 alarm 发生前的完整用户寄存器 |
| `alarm_active` | 标记 handler 是否正在执行，防止重入 |

这些字段必须属于每个进程，而不能定义成全局变量。不同进程可以设置不同周期和 handler，并且它们的计数、寄存器状态不能互相覆盖。

## 四、为什么要保存整个 trapframe

trapframe 保存用户进程进入内核时的寄存器状态，包括：

```text
epc       用户程序计数器
ra        返回地址
sp        用户栈指针
a0-a7     参数、返回值、系统调用号
s0-s11    被调用者保存寄存器
t0-t6     临时寄存器
其他通用寄存器
```

alarm 可能在任意一条用户指令后发生。如果只保存 `epc`，handler 修改的其他寄存器会破坏原程序。

因此触发时执行：

```c
*(p->alarm_trapframe) = *(p->trapframe);
```

这会复制整个 `struct trapframe`。

`alarm_trapframe` 是指针，所以必须先为它分配物理内存。否则它默认是空指针，复制时会向地址 `0` 写数据，引发：

```text
scause=0xf
stval=0x0
panic: kerneltrap
```

## 五、进程创建和释放

### `allocproc()`

创建进程时，除了普通 `trapframe`，还要分配 alarm 备份页：

```c
if((p->alarm_trapframe =
    (struct trapframe *)kalloc()) == 0){
  freeproc(p);
  release(&p->lock);
  return 0;
}
```

然后初始化 alarm 状态：

```c
p->alarm_interval = 0;
p->alarm_handle = 0;
p->alarm_tick = 0;
p->alarm_active = 0;
```

这样新进程默认没有安装 alarm。

### `freeproc()`

进程结束时释放备份页：

```c
if(p->alarm_trapframe)
  kfree((void *)p->alarm_trapframe);
p->alarm_trapframe = 0;
```

分配和释放必须配对，否则反复创建、退出进程会泄漏物理页。

## 六、注册 alarm 系统调用

### 分配系统调用号

在 `kernel/syscall.h` 中：

```c
#define SYS_sigalarm  22
#define SYS_sigreturn 23
```

系统调用号是用户态和内核态共同遵守的编号。用户态把编号放入 `a7`，内核根据 `a7` 从系统调用表中找到处理函数。

### 声明内核处理函数

在 `kernel/syscall.c` 中：

```c
extern uint64 sys_sigalarm(void);
extern uint64 sys_sigreturn(void);
```

然后加入分发表：

```c
[SYS_sigalarm]  sys_sigalarm,
[SYS_sigreturn] sys_sigreturn,
```

分发表类型是：

```c
static uint64 (*syscalls[])(void);
```

因此所有内核系统调用处理函数都必须是：

```c
uint64 sys_xxx(void);
```

不能写成：

```c
uint64 sys_sigalarm(int interval, uint64 handler);
```

否则函数指针类型与 `syscalls[]` 不兼容。

## 七、用户态声明为什么有参数

在 `user/user.h` 中：

```c
int sigalarm(int, void (*)());
int sigreturn(void);
```

这是用户程序看到的普通 C 接口。编译器需要知道参数个数和类型，才能：

1. 检查 `sigalarm(2, periodic)` 是否合法。
2. 根据 RISC-V ABI 把第一个参数放入 `a0`。
3. 把第二个参数放入 `a1`。
4. 把返回值从 `a0` 交给调用者。

如果错误地声明为：

```c
int sigalarm(void);
```

那么 `alarmtest.c` 调用两个参数时就会出现：

```text
error: too many arguments to function 'sigalarm'
```

如果把名称写成 `signalreturn`，但测试调用 `sigreturn()`，则会出现隐式声明错误。

## 八、用户态系统调用入口

在 `user/usys.pl` 中增加：

```perl
entry("sigalarm");
entry("sigreturn");
```

构建时脚本会生成类似以下汇编：

```asm
.global sigalarm
sigalarm:
  li a7, SYS_sigalarm
  ecall
  ret
```

注意：这个 stub 不需要手动搬运参数。

调用 `sigalarm(2, periodic)` 前，C 编译器已经按照 ABI 放置参数：

```text
a0 = 2
a1 = periodic 的用户虚拟地址
```

stub 只需要设置：

```text
a7 = SYS_sigalarm
```

然后执行 `ecall` 进入内核。

## 九、参数如何从用户态到内核态

以调用为例：

```c
sigalarm(2, periodic);
```

寄存器状态为：

```text
a0 = 2
a1 = periodic
a7 = SYS_sigalarm
```

执行 `ecall` 后，`trampoline.S` 把用户寄存器保存进：

```c
p->trapframe
```

`usertrap()` 判断 `scause == 8`，然后调用：

```c
syscall();
```

`syscall()` 从 trapframe 读取系统调用号：

```c
num = p->trapframe->a7;
```

再调用对应的无参内核处理函数：

```c
p->trapframe->a0 = syscalls[num]();
```

### `argint()` 和 `argaddr()`

`sys_sigalarm()` 使用：

```c
argint(0, &interval);
argaddr(1, &handler);
```

参数编号与寄存器的对应关系是：

| 参数序号 | trapframe 寄存器 |
| ---: | --- |
| 0 | `a0` |
| 1 | `a1` |
| 2 | `a2` |
| 3 | `a3` |
| 4 | `a4` |
| 5 | `a5` |

所以这两句等价于读取：

```c
interval = (int)p->trapframe->a0;
handler = p->trapframe->a1;
```

`argint()` 用于读取 32 位整数语义的参数；`argaddr()` 用于读取 64 位地址或指针参数。

## 十、`sys_sigalarm()`

实现：

```c
uint64
sys_sigalarm(void)
{
  int interval;
  uint64 handler;

  argint(0, &interval);
  argaddr(1, &handler);

  struct proc *p = myproc();
  p->alarm_interval = interval;
  p->alarm_handle = handler;
  p->alarm_tick = 0;
  return 0;
}
```

函数职责：

1. 从保存的用户寄存器中读取参数。
2. 把 alarm 配置写入当前进程。
3. 重置 tick，令新周期从当前时刻开始计算。

内核处理函数没有 C 形参，因为它不是由用户代码直接进行普通函数调用的。它由统一的 `syscall()` 分发表调用，参数要从 trapframe 中读取。

## 十一、Timer interrupt 如何触发 handler

`usertrap()` 中：

```c
if(which_dev == 2) {
  struct proc *p = myproc();

  if(p->alarm_interval > 0 && !p->alarm_active) {
    p->alarm_tick++;

    if(p->alarm_tick >= p->alarm_interval) {
      p->alarm_active = 1;
      p->alarm_tick = 0;
      *(p->alarm_trapframe) = *(p->trapframe);
      p->trapframe->epc = p->alarm_handle;
    }
  }

  yield();
}
```

### 为什么在 timer interrupt 中计数

`which_dev == 2` 表示当前 trap 是时钟中断。只有当前进程真正运行并收到 timer interrupt 时，它的计数才增加，这符合“进程消耗的 CPU 时间”语义。

### 为什么判断 `alarm_interval > 0`

`sigalarm(0, 0)` 用来关闭 alarm。若没有这个条件，周期为 0 会导致每次 timer interrupt 都触发。

### 为什么判断 `!alarm_active`

handler 自己也会运行一段时间，也可能遇到 timer interrupt。如果不阻止重入，内核会覆盖第一次保存的 trapframe，并再次进入 handler：

```text
原程序 -> handler -> handler -> handler ...
```

这会丢失原程序状态。`alarm_active` 保证 handler 完成并调用 `sigreturn()` 前不会再次触发 alarm。

### 为什么保存 trapframe 后只修改 `epc`

保存副本后：

```c
p->trapframe->epc = p->alarm_handle;
```

`usertrapret()` 最终使用 trapframe 恢复用户寄存器，并通过 `sret` 返回 `epc`。因此 CPU 回到用户态时会从 handler 开始运行。

其他寄存器暂时保持被中断时的值，使 handler 可以在正常用户栈上执行。

## 十二、`sys_sigreturn()`

实现：

```c
uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();
  uint64 a0 = p->alarm_trapframe->a0;

  *(p->trapframe) = *(p->alarm_trapframe);
  p->alarm_active = 0;

  return a0;
}
```

函数职责：

1. 恢复 alarm 发生前的完整 trapframe。
2. 清除 `alarm_active`，允许下一个周期再次触发。
3. 保持被中断时的 `a0` 不变。

### 为什么不能简单 `return 0`

所有系统调用返回后，`syscall()` 都会执行：

```c
p->trapframe->a0 = syscalls[num]();
```

`sys_sigreturn()` 虽然恢复了完整 trapframe，但如果返回 `0`，`syscall()` 紧接着会再次把 `a0` 覆盖成 `0`。

所以要先保存原来的 `a0`：

```c
uint64 a0 = p->alarm_trapframe->a0;
```

恢复 trapframe 后再返回这个值。这样 `syscall()` 写回的仍然是 alarm 发生前的 `a0`。

这正是 `alarmtest` 的 `test3` 检查内容。

## 十三、alarmtest 各项测试的含义

### `test0`

检查 handler 至少被调用一次。它验证：

- 系统调用注册正确。
- timer tick 正常计数。
- `epc` 能跳转到 handler。

### `test1`

检查 handler 可以周期性执行多次，并且返回后寄存器完全恢复。

测试中的 `i`、`j` 和指针可能被编译器保存在寄存器中。如果 trapframe 恢复不完整，最终 `i != j`。

### `test2`

handler 故意执行很长时间，检查执行期间不会再次进入 handler。它主要验证 `alarm_active` 防重入逻辑。

### `test3`

在 `a0` 中放入 `0xac`，等待 alarm 后再次读取，检查 `sigreturn()` 是否错误地把 `a0` 改成系统调用返回值。

## 十四、用户态和内核态声明为什么不同

两侧接口不同，是因为它们承担不同角色。

用户态声明：

```c
int sigalarm(int interval, void (*handler)());
```

作用是让 C 编译器按照 ABI 传递参数。用户态的调用看起来像普通函数调用。

内核态声明：

```c
uint64 sys_sigalarm(void);
```

作用是满足统一系统调用分发表的函数指针类型。参数已经通过 trap 保存到 `p->trapframe`，内核函数使用 `argint()` 和 `argaddr()` 获取。

对应关系为：

```text
用户态 C 参数
  interval --------------------> a0
  handler ---------------------> a1

用户态 stub
  SYS_sigalarm ----------------> a7

ecall / trampoline
  a0、a1、a7 ------------------> p->trapframe

内核态
  argint(0, &interval) <-------- trapframe->a0
  argaddr(1, &handler) <-------- trapframe->a1
  syscall number <-------------- trapframe->a7
```

因此“用户态有参数、内核处理函数无 C 参数”并不矛盾。参数没有消失，只是通过寄存器和 trapframe 跨越了特权级边界。

## 十五、如何在 xv6 中实现一个系统调用

以新增 `foo(int value)` 为例，通用步骤如下。

### 1. 定义系统调用号

在 `kernel/syscall.h` 中添加唯一编号：

```c
#define SYS_foo 24
```

### 2. 声明用户态接口

在 `user/user.h` 中添加：

```c
int foo(int);
```

这里的参数和返回类型必须符合用户程序实际调用方式。

### 3. 生成用户态 stub

在 `user/usys.pl` 中添加：

```perl
entry("foo");
```

生成的 stub 会把 `SYS_foo` 写入 `a7` 并执行 `ecall`。

### 4. 实现内核处理函数

通常放在 `kernel/sysproc.c` 或 `kernel/sysfile.c`：

```c
uint64
sys_foo(void)
{
  int value;
  argint(0, &value);

  // 完成内核操作。
  return 0;
}
```

所有系统调用处理函数保持 `uint64 sys_xxx(void)` 形式。

### 5. 在内核中声明并注册

在 `kernel/syscall.c` 中：

```c
extern uint64 sys_foo(void);
```

并加入分发表：

```c
[SYS_foo] sys_foo,
```

### 6. 将测试程序加入文件系统

如果新建了 `user/footest.c`，还要在 Makefile 的 `UPROGS` 中添加：

```make
$U/_footest
```

否则源文件虽然存在，xv6 的 `fs.img` 中没有对应程序，shell 会显示：

```text
exec footest failed
```

### 7. 构建并验证

```sh
make qemu
```

进入 xv6 shell 后运行测试程序。出现 `unknown sys call` 时优先检查系统调用号、`usys.pl` 和 `syscalls[]`；出现编译参数错误时优先检查 `user/user.h` 与 `sys_xxx(void)` 的职责是否混淆。

## 十六、实现检查清单

- `kernel/syscall.h` 中编号唯一。
- `user/user.h` 中用户态原型正确。
- `user/usys.pl` 中存在同名 entry。
- `kernel/syscall.c` 中存在 `extern` 声明。
- `kernel/syscall.c` 的 `syscalls[]` 已注册。
- 内核处理函数签名为 `uint64 sys_xxx(void)`。
- 参数通过 `argint`、`argaddr` 或 `argstr` 获取。
- 返回值通过 `a0` 返回，注意是否需要保留原 `a0`。
- 新进程字段在创建时初始化，在退出时释放资源。
- 用户测试程序已加入 Makefile 的 `UPROGS`。
- 正常路径、失败路径和重复调用均经过测试。

## 十七、当前验证结果

当前实现运行 `alarmtest` 的结果为：

```text
test0 passed
test1 passed
test2 passed
test3 passed
```
