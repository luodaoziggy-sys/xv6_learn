# xv6 Page Table Lab 实现总结

本文总结当前 `pgtbl` 分支中的实现思路、修改文件和主要函数。内容包括：

1. 打印页表。
2. 通过只读共享页加速 `getpid()`。
3. 分配、映射、复制和释放 2MB 大页。

## 一、Sv39 页表基础

xv6 在 RISC-V Sv39 模式下使用三级页表：

| 页表级别 | 一个叶 PTE 映射的范围 | 虚拟地址索引位 |
| --- | ---: | --- |
| level 2 | 1GB | 30-38 |
| level 1 | 2MB | 21-29 |
| level 0 | 4KB | 12-20 |

普通页映射会一直遍历到 level 0，再写入一个叶 PTE。2MB 大页映射会在 level 1 提前写入叶 PTE，因此一个 PTE 可以覆盖连续的 512 个 4KB 页。

判断一个 PTE 是页表指针还是叶节点，使用：

```c
#define PTE_LEAF(pte) (((pte) & PTE_R) | ((pte) & PTE_W) | ((pte) & PTE_X))
```

- 只有 `PTE_V`，没有 `R/W/X`：指向下一级页表。
- 包含 `R/W/X` 中任意权限：叶 PTE，直接描述内存映射。

## 二、修改文件概览

| 文件 | 修改内容 |
| --- | --- |
| `kernel/memlayout.h` | 定义 `USYSCALL`、大页池容量和 2MB 大小 |
| `kernel/proc.h` | 在 `struct proc` 中保存进程的 `usyscall` 页 |
| `kernel/proc.c` | 分配、映射和释放 `USYSCALL` 页 |
| `kernel/kalloc.c` | 从物理内存中预留连续 2MB 块，并提供大页分配器 |
| `kernel/vm.c` | 支持大页遍历、映射、地址转换、复制和解除映射 |
| `kernel/defs.h` | 声明新增的跨文件函数 |

## 三、USYSCALL 共享页

### 设计思路

普通 `getpid()` 需要通过 `ecall` 从用户态进入内核态。实验要求为每个进程建立一个用户态只读页，内核把 PID 写入该页，用户程序可以直接读取，避免系统调用切换。

该页固定映射在：

```text
TRAMPOLINE
TRAPFRAME
USYSCALL
```

### `allocproc()`

```c
if((p->t = (struct usyscall *)kalloc()) == 0) {
  ...
}
p->t->pid = p->pid;
```

函数职责：创建进程时分配一个物理页，并把当前进程 PID 写入 `struct usyscall`。

### `proc_pagetable()`

```c
mappages(pagetable, USYSCALL, PGSIZE,
         (uint64)p->t, PTE_R | PTE_U);
```

函数职责：把 `p->t` 映射到用户虚拟地址 `USYSCALL`。只设置 `PTE_R | PTE_U`，用户可以读取，但不能修改 PID。

### `freeproc()` 和 `proc_freepagetable()`

- `freeproc()` 使用 `kfree()` 释放 `p->t` 对应的物理页。
- `proc_freepagetable()` 使用 `uvmunmap()` 删除 `USYSCALL` 虚拟映射，但不在这里重复释放物理页。

## 四、页表打印

### `vmprint()`

入口函数，先打印根页表地址，再调用递归函数：

```c
void
vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", pagetable);
  vmsprint(pagetable, 0);
}
```

### `vmsprint()`

函数职责：遍历当前页表中的 512 个 PTE，打印有效项，并递归打印下一级页表。

核心逻辑：

```c
for(int i = 0; i < 512; i++) {
  pte_t pte = pagetable[i];
  if(pte & PTE_V) {
    // 按层级打印缩进、PTE 和物理地址。
    // 非叶 PTE 才应该继续递归。
  }
}
```

递归时应区分叶 PTE 和页表指针。大页出现后，level 1 也可能是叶节点，不能把它指向的物理内存当成下一级页表继续遍历。

## 五、2MB 物理内存分配器

### 数据结构

```c
void *superblock[KALLOC_2M];
```

`superblock` 是一个简单的大页空闲数组：

- 非零元素表示一个可用的、2MB 对齐的连续物理块。
- 零元素表示该槽位中的大页已被分配。
- 当前 `KALLOC_2M` 为 16，可以保留 32MB 大页物理内存。

### `freerange_2M()`

函数职责：初始化物理内存分配器，同时建立普通页池和大页池。

处理流程：

1. 从内核结束地址后的第一个 4KB 对齐地址开始扫描。
2. 如果当前物理地址 2MB 对齐、后面仍有完整 2MB，并且大页池未满，就把这块内存放入 `superblock`。
3. 否则调用 `kfree()`，把当前 4KB 页放入普通空闲链表。

必须保证大页物理地址 2MB 对齐，否则 RISC-V 无法建立合法的 level 1 叶映射。

### `kalloc_2M()`

函数职责：从 `superblock` 中取出一个空闲大页。

```c
acquire(&kmem.lock);
// 找到非零槽位，取出指针并将槽位置零。
release(&kmem.lock);
```

操作数组时复用 `kmem.lock`，避免多个 CPU 同时分配同一个大页。分配成功后，用调试字节填充整个 2MB 区域。

### `superfree()`

函数职责：把不再使用的 2MB 物理块放回大页池。

它寻找第一个空槽并写入物理地址。该函数必须与 `kalloc_2M()` 配对，不能使用普通 `kfree()` 释放大页，否则只有其中一个 4KB 页会回到普通页链表。

## 六、大页页表遍历

### `walklevel()`

函数签名：

```c
static pte_t *
walklevel(pagetable_t pagetable, uint64 va,
          int alloc, int *leaflevel);
```

函数职责：查找虚拟地址对应的叶 PTE，同时通过 `leaflevel` 返回叶节点所在层级。

遍历逻辑：

1. 从 level 2 开始查找对应 PTE。
2. 如果 PTE 有效并且是叶节点，立即返回，不再进入下一级。
3. 如果 PTE 有效但不是叶节点，把其中的物理地址作为下一级页表。
4. 如果 PTE 无效且 `alloc != 0`，分配新的页表页。
5. 最终到达 level 0 时，返回普通页 PTE。

`leaflevel` 的含义：

- `0`：普通 4KB 页。
- `1`：2MB 大页。

原有 `walk()` 继续保留，内部调用 `walklevel()`，从而保持其他代码的接口不变。

## 七、大页映射

### `mapsuperpage()`

函数职责：建立一个真正的 Sv39 level 1 大页映射。

处理流程：

1. 检查虚拟地址和物理地址是否都按 2MB 对齐。
2. 找到根页表中的 level 2 PTE。
3. 如果 level 1 页表不存在，分配一个普通 4KB 页作为 level 1 页表。
4. 找到 level 1 中对应的 PTE。
5. 直接写入 `PA2PTE(pa) | perm | PTE_V`，使其成为叶 PTE。

它与 `mappages()` 的核心区别是：

```text
mappages():      level 2 -> level 1 -> level 0 leaf，重复 512 次
mapsuperpage():  level 2 -> level 1 leaf，只写 1 次
```

这也是 `superpg_test` 要求同一 2MB 范围内所有地址返回相同 PTE 的原因。

## 八、分配用户内存

### `uvmalloc()`

函数职责：扩大用户地址空间，并尽可能使用 2MB 大页。

使用大页需要同时满足：

```c
a % KALLOC_2M_SIZE == 0 &&
a + KALLOC_2M_SIZE <= newsz
```

即当前虚拟地址已经 2MB 对齐，并且待分配空间至少还有完整的 2MB。

满足条件时：

1. 调用 `kalloc_2M()` 分配连续物理内存。
2. 清零整个 2MB 区域。
3. 调用 `mapsuperpage()` 建立 level 1 映射。
4. 循环步长设置为 2MB。

不满足条件时继续使用 `kalloc()` 和 `mappages()` 分配普通 4KB 页。因此，一个进程可以同时包含普通页和大页。

## 九、虚拟地址转换

### `walkaddr()`

普通页 PTE 存放 4KB 页的物理起始地址，而大页 PTE 存放 2MB 块的物理起始地址。找到 level 1 叶 PTE 后，还必须加上虚拟地址在大页中的偏移：

```c
pa = PTE2PA(*pte);
if(level == 1)
  pa += va & (SUPERPGSIZE - 1);
```

例如：

```text
虚拟大页起点：0x400000
当前虚拟地址：0x401234
物理大页起点：0x86000000
大页内偏移：  0x1234
最终物理地址：0x86001234
```

如果不加偏移，2MB 范围内的所有虚拟地址都会错误地落到物理大页起点附近。

## 十、fork 复制大页

### `uvmcopy()`

函数职责：复制父进程的用户内存和映射到子进程。

对于普通页：

1. 使用 `kalloc()` 分配 4KB。
2. 使用 `memmove()` 复制 4KB。
3. 使用 `mappages()` 建立 level 0 映射。

对于 level 1 大页：

1. 使用 `kalloc_2M()` 分配 2MB。
2. 使用 `memmove()` 复制完整 2MB。
3. 使用 `mapsuperpage()` 为子进程建立 level 1 映射。
4. 将循环步长设置为 2MB，避免对同一个大页重复处理 512 次。

判断依据必须是 `walklevel()` 返回的层级，不能只判断虚拟地址或物理地址是否 2MB 对齐。地址对齐不代表它一定是大页映射。

## 十一、解除映射和释放

### `uvmunmap()`

函数职责：删除用户页映射，并在需要时释放对应物理内存。

通过 `walklevel()` 判断映射类型：

- level 0：调用 `kfree()`，循环前进 4KB。
- level 1：调用 `superfree()`，清除一级叶 PTE，循环前进 2MB。

清除 PTE：

```c
*pte = 0;
```

这是避免 `panic: freewalk: leaf` 的关键。

### `freewalk()`

函数职责：递归释放页表结构本身，而不是释放用户数据页。

正确的释放顺序是：

```text
uvmfree()
  -> uvmunmap()  删除叶映射并释放用户数据页
  -> freewalk()  释放已经没有叶节点的各级页表页
```

如果 `freewalk()` 仍看到有效叶 PTE，说明前面的解除映射不完整，因此会触发：

```text
panic: freewalk: leaf
```

## 十二、错误原因总结

### `pte different`

最初虽然分配了连续 2MB 物理内存，但仍通过 `mappages()` 建立了 512 个 level 0 PTE。测试查询每个 4KB 地址时得到不同 PTE，因此失败。

修复方法：使用 `mapsuperpage()` 在 level 1 建立一个叶 PTE。

### 启动时 `kerneltrap`

最初通过“地址是否 2MB 对齐”判断映射是否为大页。虚拟地址 0 虽然对齐，但它通常是普通 4KB 页，导致 `fork()` 错误地复制 2MB 并越界访问。

修复方法：让页表遍历函数直接返回叶 PTE 的真实层级。

### `fork` 返回失败

父进程申请约 8MB 后占用了多个大页，子进程复制时也需要相同数量。大页池只有 4 个时无法完成复制。

修复方法：将大页池扩大到 16 个槽位。

### `panic: freewalk: leaf`

大页位于 level 1，原来的解除映射逻辑只按普通 level 0 页处理，没有正确清除一级叶 PTE。

修复方法：`uvmunmap()` 根据实际叶层级选择释放函数和循环步长，并在释放后清零 PTE。

## 十三、完整生命周期

```text
系统启动
  -> freerange_2M()
  -> 建立普通 4KB 页池和 2MB 大页池

sbrk() 扩大进程内存
  -> uvmalloc()
  -> 对齐且空间足够时调用 kalloc_2M()
  -> mapsuperpage() 建立 level 1 叶映射

访问用户地址
  -> walklevel() 找到叶 PTE 和层级
  -> walkaddr() 加上大页内偏移

fork()
  -> uvmcopy()
  -> 分配并复制新的 2MB 物理块
  -> 为子进程建立新的 level 1 映射

进程退出或缩小内存
  -> uvmunmap()
  -> superfree() 回收 2MB 物理块
  -> 清除 level 1 叶 PTE
  -> freewalk() 释放页表结构
```

## 十四、验证结果

运行：

```sh
make qemu
```

进入 xv6 shell 后执行：

```sh
pgtbltest
```

当前验证结果：

```text
superpg_test: OK
pgtbltest: all tests succeeded
```
