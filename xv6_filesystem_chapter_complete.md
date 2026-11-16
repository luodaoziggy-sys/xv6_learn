# xv6 文件系统（File System）完整学习笔记

> 基于 xv6-riscv 的经典文件系统实现（`kernel/fs.c`、`kernel/bio.c`、`kernel/log.c`、`kernel/sysfile.c` 等）。从磁盘布局、Bitmap、Inode、Buffer Cache、目录与路径解析到预写日志，最后逐步追踪 `echo "hi" > x`。本文中 `Block 32/33/45/46/595` 的角色属于**结合教学示例的可能解释**，必须以当前 `mkfs` 布局和运行时输出核实；截图只能证明写入顺序，不能单凭数字证明块类型。

## 0. 先建立一张完整的心智地图

```text
用户程序：sh 执行 echo "hi" > x
             │
             ▼
系统调用层：sys_open / sys_write / sys_close （sysfile.c）
             │
             ▼
路径层：nameiparent / namei → namex → skipelem
             │
             ▼
目录层：dirlookup / dirlink（目录文件里的 dirent）
             │
             ▼
Inode 层：ialloc / iget / ilock / iupdate / iput
             │
             ▼
数据层：writei / readi → bmap → balloc / bfree
             │
             ▼
Buffer Cache：bread / bget / brelse（bio.c）
             │
             ▼
日志：begin_op / log_write / end_op → commit（log.c）
             │
             ▼
VirtIO 驱动：virtio_disk_rw → 磁盘
```

注意此图是**依赖关系**，不是一次系统调用的严格顺序。`begin_op()` 实际在文件系统修改开始前调用；`log_write()` 在多个层级穿插执行；`end_op()` 在操作完成后调用。

---

## 1. 磁盘布局与 Superblock

xv6 把磁盘分成多个连续的 **1024 B 块**：

```text
[Boot][Superblock][Log blocks][Inode blocks][Bitmap blocks][Data blocks]
```

- **Boot Block**：保留给启动相关内容。
- **Superblock**：保存文件系统整体布局和参数。
- **Log Blocks**：事务日志，一个日志头块和多个日志数据块。
- **Inode Blocks**：存放磁盘 inode，即 `struct dinode`。
- **Bitmap Blocks**：每 bit 描述一个磁盘块是否已分配。
- **Data Blocks**：既可能存普通文件的字节，也可能存目录的 `dirent` 数组，或一级间接索引表。

`kernel/fs.h` 中的超级块：

```c
struct superblock {
  uint magic;
  uint size;        // 文件系统总块数
  uint nblocks;     // 数据区块数
  uint ninodes;     // inode 数
  uint nlog;        // 日志区域总块数（含头块）
  uint logstart;    // 日志区域起始块号
  uint inodestart;  // inode 区域起始块号
  uint bmapstart;   // bitmap 区域起始块号
};
```

`readsb()` 使用 `bread(dev, 1)` 获取 Block 1，把开头的 `sizeof(struct superblock)` 字节复制到内存全局变量 `sb`。`fsinit()` 检查 `FSMAGIC`，并调用 `initlog()`。**磁盘第 1 块是超级块，内存中的 `sb` 只是其副本**。

### 三个“块号”不要混淆

| 概念 | 例子 | 意义 |
|---|---|---|
| 磁盘物理块号 | 595 | 文件系统中真实的磁盘 Block 编号 |
| 文件逻辑块号 `bn` | 0 | 文件内部第几个数据块，从 0 开始 |
| inode 编号 `inum` | 20 | inode 表中的编号，并不是磁盘块号 |

---

## 2. Buffer Cache：磁盘块的统一缓存

在 `kernel/bio.c` 中：

```c
struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct buf head;
} bcache;
```

- 每个 `struct buf` 有 `dev`、`blockno`、`valid`、`refcnt`、`data[BSIZE]`、睡眠锁以及双向链表指针。
- **不管磁盘块属于 Inode、Bitmap、Log、Superblock 还是 Data，`bread()` 都以整块方式缓存它。**
- `bget(dev, blockno)`：先找已经缓存的 `(dev, blockno)`；找不到则从 LRU 链表尾部找 `refcnt == 0` 的 Buffer 复用。返回时持有 Buffer 的睡眠锁。
- `bread()`：`bget()` 后如果 `valid == 0`，执行 `virtio_disk_rw(b, 0)` 读盘；否则复用缓存。
- `brelse()`：释放睡眠锁，`refcnt--`；当减少到 0 时，摘下链表节点并头插，记录“最近完成使用”。**它不写盘，也不清空 Buffer。**
- `bwrite()`：通过 `virtio_disk_rw(b, 1)` 把 Buffer 整块写到它对应的磁盘位置，调用者必须持有睡眠锁。

### `refcnt` 和 LRU

`refcnt` **不是访问频率**，只是当前引用次数：`0` 表示可复用；`>0` 表示不能复用。LRU 链表位置反映最近完成使用的次序。`log_write()` 的 `bpin()` 会额外增加引用数，防止尚未提交的修改被回收。

### 为什么还有独立的 Inode Cache？

```c
// kernel/fs.c
struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} itable;
```

- **Buffer Cache** 缓存完整的 Inode Block（1024 B，里面有多个磁盘 `dinode`）。
- **Inode Cache** 缓存“一个个内存 `struct inode`”，带 `ref`、`valid` 和锁。
- `iget()` 只获取或建立内存 inode 表项，不负责从磁盘读全套元数据；`ilock()` 在 `valid == 0` 时读取相应 Inode Block，并将其中一个 `dinode` 的字段复制到 `struct inode`。

---

## 3. Bitmap：磁盘块如何分配与释放

### 3.1 一个 Bitmap Block 管多少块？

```c
#define BSIZE 1024
#define BPB (BSIZE*8)                       // 8192
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)
```

每一个 bit 管理一个磁盘块：`0` 空闲，`1` 已分配。一个 1024 B Bitmap Block 管理 8192 个磁盘块，因此 Bitmap 区域可以由 **1 个或多个块**组成。

例如待操作物理块 `b = 8202`：

```text
b / 8192 = 1        → 第 2 个 Bitmap Block
b % 8192 = 10       → 在该 Bitmap Block 内的第 10 个 bit
bi / 8 = 1          → 第 2 个字节
bi % 8 = 2          → 该字节内的 bit 2
m = 1 << 2 = 0b00000100
```

### 3.2 `balloc(dev)`：分配空闲块

核心代码（省略错误与循环边界）：

```c
for (b = 0; b < sb.size; b += BPB) {
  bp = bread(dev, BBLOCK(b, sb));
  for (bi = 0; bi < BPB && b + bi < sb.size; bi++) {
    m = 1 << (bi % 8);
    if ((bp->data[bi/8] & m) == 0) {
      bp->data[bi/8] |= m;    // 设置 bit = 1
      log_write(bp);          // 登记 Bitmap Block 被修改
      brelse(bp);
      bzero(dev, b + bi);     // 新分配磁盘块置零，也登记日志
      return b + bi;         // 返回物理磁盘块号
    }
  }
  brelse(bp);
}
return 0;
```

这里 `b` 是该 Bitmap Block 管理范围的起始磁盘块号；`bi` 是范围内部的 bit 偏移，真实磁盘块号为 `b + bi`。分配操作不仅修改 Bitmap，还会通过 `bzero()` 初始化新块的数据内容。

### 3.3 `bfree(dev, b)`：释放块

```c
bp = bread(dev, BBLOCK(b, sb));
bi = b % BPB;
m = 1 << (bi % 8);
if ((bp->data[bi/8] & m) == 0)
  panic("freeing free block");
bp->data[bi/8] &= ~m;  // 清除 bit = 0
log_write(bp);
brelse(bp);
```

`bfree()` 修改的是**分配状态**，不会擦除数据块里的旧字节。也不要误以为 xv6 有“inode bitmap”：经典 xv6 的 `ialloc()` 是检查磁盘 `dinode.type == 0` 找空闲 inode。

---

## 4. Inode：从磁盘元数据到内存表示

### 4.1 磁盘 `dinode` 的大小

```c
struct dinode {
  short type;            // 2 B
  short major;           // 2 B
  short minor;           // 2 B
  short nlink;           // 2 B
  uint size;             // 4 B
  uint addrs[NDIRECT+1]; // 经典版本：13 × 4 B = 52 B
};
```

在 `NDIRECT == 12` 的经典实现中，合计 **64 B**，故 `IPB = 1024 / 64 = 16`：一个 Inode Block 保存 16 个磁盘 inode。若你做了 bigfile Lab 并改过定义，需以当前 `fs.h` 为准。

### 4.2 inode 20 在哪里？

```c
#define IBLOCK(i, sb) ((i) / IPB + sb.inodestart)
bp = bread(dev, IBLOCK(inum, sb));
dip = (struct dinode*)bp->data + inum%IPB;
```

`inum=20`，`IPB=16`：在 inode 区域的第 2 个磁盘块，**该块内索引 4**（从零开始），偏移 `4 × 64 = 256` B。`(struct dinode*)` 只是让指针算术按 `sizeof(struct dinode)` 前进，并没有复制数据。

### 4.3 初始化、分配、读取、写回

| 函数 | 做什么 | 是否直接获取完整磁盘 inode 信息 |
|---|---|---|
| `iinit()` | 初始化 `itable.lock` 和每个 inode 的睡眠锁 | 否 |
| `ialloc()` | 扫描磁盘 `dinode.type==0`，`memset` 清零，设置 `type`，`log_write()` | 是，操作 `dinode` 所在块 |
| `iget()` | 在 `itable` 查找或占用内存表项，增加 `ref`，设 `valid=0` | 否 |
| `ilock()` | 获取睡眠锁；若 `valid==0`，读 Inode Block 并复制元数据 | 需要时是 |
| `iupdate()` | 将内存 inode 的 `type/size/nlink/addrs[]` 等写回 `dinode` 对应的 Buffer，并 `log_write()` | 写回 |
| `idup()` / `iput()` | 增加 / 减少内存引用 | 按情况 |
| `iunlock()` | 释放 inode 睡眠锁 | 否 |
| `itrunc()` | 释放文件所有数据块及间接索引块，size=0，更新 inode | 是 |

**区分 `ref` 和 `nlink`**：`ref` 是**内存中**对 inode 的引用（打开文件/工作目录等）；`nlink` 是磁盘上指向该 inode 的**目录链接数**。只有没有链接、并且最后的引用释放时，`iput()` 才能真正回收 inode 及文件数据。

---

## 5. 文件数据：`addrs[]`、`bmap()`、`readi()`、`writei()`

### 5.1 文件内偏移如何定位磁盘地址？

```c
bn       = off / BSIZE;   // 文件内部的逻辑块号
in_block = off % BSIZE;   // 逻辑块中的字节偏移
addr     = bmap(ip, bn);  // 逻辑块号 → 磁盘物理块号
bp       = bread(ip->dev, addr);
```

例如 `off=2500`：`bn=2`，块内字节偏移 `452`。若 `ip->addrs[2]=595`，则读/写 `Block 595` 的 `bp->data+452`。

### 5.2 直接块与一级间接块

经典 xv6（未做 bigfile 扩展）中：

```text
inode.addrs[0 .. 11]  → 12 个直接数据块地址
inode.addrs[12]      → 1 个间接索引块地址
                           └→ 256 个 uint 块地址
```

- `bn < NDIRECT`：直接访问 `ip->addrs[bn]`，如果值为 0，`balloc()` 分配新数据块。
- `bn >= NDIRECT`：`bn -= NDIRECT` 后，先获得一级间接块，再以 `a[bn]` 查真实数据块号；缺少间接索引块或数据块时也会分配。
- `bmap()` 在此版本中**不仅查找，还可能分配**，因此不要把它看成一个纯查表函数。

### 5.3 `readi()` 与 `writei()`

`readi(ip, user_dst, dst, off, n)`：检查 `off` 和 `size`，逐块 `bmap() → bread() → either_copyout()`，最后 `brelse()`；一般不修改磁盘。

`writei(ip, user_src, src, off, n)`：检查边界，逐块 `bmap() → bread() → either_copyin()`，每个修改后的块调用 `log_write()`，最后更新 `ip->size` 并 `iupdate()`。若数据块原来未分配，则 `bmap→balloc` 会连带修改 **Bitmap** 和 inode 的 `addrs[]`。

---

## 6. 目录与路径解析：文件名为何写入 Data Block？

`struct dinode` / `struct inode` **不保存文件名**。名字在目录文件的数据区：

```c
struct dirent {
  ushort inum;
  char name[DIRSIZ];
};
```

例如根目录数据块里：

```text
"."     → inode 1
".."    → inode 1
"README"→ inode 2
"x"     → inode 20     // 创建 x 后新增
```

- `skipelem()`：逐项拆分 `/a/b/x`。
- `namex()`：从根目录或当前目录开始，逐级 `ilock → dirlookup → iunlockput`。
- `namei(path)`：返回目标文件的 inode。
- `nameiparent(path, name)`：返回父目录 inode，把最后一级名字写进 `name`。创建文件需要这个接口。
- `dirlookup(dp, "x", ...)`：逐项 `readi()`，找到 `dirent.name=="x"`，由 `inum` 调 `iget()`。
- `dirlink(dp, "x", inum)`：检查不重名，找空目录项或追加，最后 `writei(dp, ...)` 修改**父目录数据块**。

因此在示例中，如果 Block 46 是根目录数据块，那么创建 `x` 必须写 Block 46，**写的是 `x → inum`，不是 `hi\n`**。

---

## 7. Logging：为什么不能随意直接 `bwrite()`？

创建文件可能要同时改变：Inode Block、Bitmap Block、目录 Data Block 和新文件 Data Block。如果写了一半断电，磁盘就可能处于不一致状态。

xv6 使用**物理重做日志（redo log）**：先在内存修改 Buffer，并由 `log_write()` 登记块号；提交时把**每个块的最终 1024 B 内容**复制到磁盘日志；写入有效日志头作为提交点；然后将日志安装到原始磁盘块。已提交日志可重复重放（幂等性）。

### 7.1 磁盘上的日志布局

假设 `log.start=2`：

```text
磁盘 Block 2：Log Header（n 和原始块号 block[]）
磁盘 Block 3：Log Data 0（原始块 block[0] 的最终内容）
磁盘 Block 4：Log Data 1（原始块 block[1] 的最终内容）
磁盘 Block 5：Log Data 2（原始块 block[2] 的最终内容）
...
```

Log Header 记录的是**原始磁盘块号**，Log Data Blocks 存放完整块内容。若 `n=3, block=[33,46,32]`，则 Log Block 3→原始 Block 33，Log Block 4→46，Log Block 5→32。

### 7.2 `begin_op()`：进入文件系统操作并预留容量

核心逻辑：

```c
void begin_op(void) {
  acquire(&log.lock);
  while (1) {
    if (log.committing)
      sleep(&log, &log.lock);
    else if (log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > LOGSIZE)
      sleep(&log, &log.lock);
    else {
      log.outstanding++;
      release(&log.lock);
      break;
    }
  }
}
```

- `outstanding` 是**尚未调用 `end_op()` 的文件系统操作数量**，不是磁盘块数量。
- `MAXOPBLOCKS` 是对每个操作可能修改块数的**保守上限预留**，不是该操作一定会修改的实际块数。
- 若日志正在提交或最坏情况下的空间不足，就 `sleep()`；等提交结束后被 `wakeup()` 唤醒重试。
- 教学版本的日志大小检查以具体 `log.c` 为准；日志还必须预留 header 块。

### 7.3 `log_write(bp)`：登记块号，做日志吸收

```c
void log_write(struct buf *b) {
  int i;
  acquire(&log.lock);
  // 略去日志空间、事务状态检查
  for (i = 0; i < log.lh.n; i++)
    if (log.lh.block[i] == b->blockno)
      break;
  log.lh.block[i] = b->blockno;
  if (i == log.lh.n) {
    bpin(b);
    log.lh.n++;
  }
  release(&log.lock);
}
```

- **第一次修改某块**：记块号，`bpin()` 使 Buffer 不能被复用，`n++`。
- **同事务内重复修改同一块**：不重复登记、不再增加 `n`；提交时读取该 Buffer 的**最终内容**。这叫 **log absorption**。
- `log_write()` **不会立即执行日志磁盘写入**。

### 7.4 `end_op()`：最后一个操作负责提交

```c
void end_op(void) {
  int do_commit = 0;
  acquire(&log.lock);
  log.outstanding--;
  if (log.committing)
    panic("log.committing");
  if (log.outstanding == 0) {
    do_commit = 1;
    log.committing = 1;
  } else {
    wakeup(&log);
  }
  release(&log.lock);

  if (do_commit) {
    commit();             // 不持有 log.lock 执行可能阻塞的磁盘 I/O
    acquire(&log.lock);
    log.committing = 0;
    wakeup(&log);
    release(&log.lock);
  }
}
```

多个并发操作可以共享同一批日志记录：只有 **`outstanding` 从 1 变为 0**，才触发 `commit()`。`log.committing=1` 阻止新操作在提交期间混入。

### 7.5 `commit()`：日志提交的四步

```c
static void commit() {
  if (log.lh.n > 0) {
    write_log();         // ① Buffer → 磁盘 Log Data Blocks
    write_head();        // ② 写磁盘 Log Header，事务正式提交
    install_trans(0);    // ③ Log Data Blocks → 原始磁盘块
    log.lh.n = 0;
    write_head();        // ④ 清空磁盘 Header，表示无需恢复
  }
}
```

- `write_log()`：用 `log.lh.n` 遍历已登记块，复制整个 `BSIZE=1024` 字节，并逐块 `bwrite` 到日志区域。
- `write_head()`：把 `n` 和 `block[]` 写到 **Log Header Block**；**此时才产生持久的提交记录**。
- `install_trans()`：将日志块内容覆盖写回原始块，并 `bunpin()` 释放事务额外引用。
- 末尾再把 `n=0` 写到磁盘日志头，解除待恢复标记。

### 7.6 Crash 时的两种结局

| Crash 时刻 | 重启恢复 |
|---|---|
| `write_head()` 提交前 | 没有有效提交记录，**不安装**本次事务（原始块仍是旧状态） |
| `write_head()` 提交后，`install_trans()` 部分完成 | 重放已提交日志，**所有修改最终生效** |

`install_trans()` 使用“整块覆盖写”，同一块被写多次最终内容不变，这是日志重放的**幂等性**。以上依赖 xv6 所假设的磁盘写入完成、顺序及日志头更新原子性；真实硬件还需关注刷盘与写缓存语义。

---

## 8. 结合截图：`bwrite 3、4、5、2、33、46、32...` 到底是什么？

你提供的截图可读到以下前两组完整输出：

```text
init: starting sh
$ echo hi > x
bwrite 3
bwrite 4
bwrite 5
bwrite 2
bwrite 33
bwrite 46
bwrite 32
bwrite 2
bwrite 3
bwrite 4
bwrite 5
bwrite 2
bwrite 45
bwrite 595
bwrite 33
bwrite 2
...
```

假设**在这个镜像中 `log.start=2`**，这些 `bwrite` 输出与两次 `commit()` 的典型形态完全一致：

### 第一组：一次已提交的三个块的修改

```text
bwrite 3    ← 将第一个修改块的新内容写到 Log Data 0
bwrite 4    ← 写到 Log Data 1
bwrite 5    ← 写到 Log Data 2
bwrite 2    ← 写 Log Header（提交点，n=3）
bwrite 33   ← 安装到原始 Block 33
bwrite 46   ← 安装到原始 Block 46
bwrite 32   ← 安装到原始 Block 32
bwrite 2    ← 清空 Log Header（n=0）
```

该批次的日志头可以理解为 `n=3, block=[33,46,32]`。注意：**前面的 `3,4,5` 是日志区的数据块号；后面的 `33,46,32` 是本次真正修改的原始块号。**

### 第二组：另一次已提交的三个块的修改

```text
bwrite 3    ← Log Data 0
bwrite 4    ← Log Data 1
bwrite 5    ← Log Data 2
bwrite 2    ← 提交 Header（n=3）
bwrite 45   ← 安装到原始 Block 45
bwrite 595  ← 安装到原始 Block 595
bwrite 33   ← 安装到原始 Block 33
bwrite 2    ← 清除 Header（n=0）
```

该批次可理解为 `n=3, block=[45,595,33]`。一种与创建小文件相符的解释是：Block 45 是 Bitmap，Block 595 是新文件 Data Block，Block 33 是记录新文件元数据的 Inode Block；**但这些类型不能仅凭这张截图断定**，必须结合 `sb.bmapstart`、`sb.inodestart`、`IBLOCK(inum,sb)` 和 inode 的 `addrs[]` 来核对。

### 如何验证块号的真正角色？

1. 在 `fsinit()` 打印 `sb.logstart, sb.nlog, sb.inodestart, sb.bmapstart, sb.size`。
2. 在 `ialloc()` 或 `iupdate()` 打印 `inum` 和 `IBLOCK(inum,sb)`。
3. 在 `dirlink()` 打印 `dp->inum`、目录 `writei()` 使用的真实块号。
4. 在 `balloc()` 打印 `b+bi`，核对是否真的分配了 Block 595。
5. 在 `log_write()` 打印 `b->blockno` 和当时 `log.lh.n`，识别每次登记顺序。注意在日志锁内打印只用于调试，勿据此推断精确时序。

此外，截图最后还有后续 `bwrite`，不能把全部行都强行归为同一个事务；Shell 执行、文件关闭以及其他文件系统操作都可能触发额外事务。具体以完整输出和对应代码为准。

---

## 9. 完整追踪：`echo "hi" > x`

**前提**：当前工作目录是 `/`，文件 `x` 最初不存在，执行的 xv6 Shell 使用 `>` 做输出重定向，`echo` 输出 `hi` 加换行（实际写入 **3 B：`'h' 'i' '\n'`**）。若 `x` 原本存在，流程会转为打开并截断，目录新增与 inode 分配步骤不适用。

### 阶段 A：Shell 解释命令与重定向

```text
Shell 解析：echo "hi" > x
    ├─ 命令：echo
    ├─ 参数：hi
    └─ stdout 重定向到文件 x
```

Shell 的子进程执行重定向：通常先 `close(1)`，再 `open("x", O_WRONLY|O_CREATE|O_TRUNC)`，因最小可用描述符为 1，所以新文件成为标准输出；然后 `exec("echo", ...)`。`echo` 调用 `write(1, "hi\n", 3)`，此时写入目的地不是终端，而是 `x`。

### 阶段 B：`open()` 创建文件 x（一个文件系统操作）

典型调用依赖链：

```text
sys_open()
  └─ begin_op()
      └─ create("x", T_FILE, ...)
          ├─ nameiparent("x", name)
          │    └─ 取得父目录 / 的 inode，name="x"
          ├─ ilock(dp)
          ├─ dirlookup(dp, "x", 0) → 没有此目录项
          ├─ ialloc(dev, T_FILE)
          │    ├─ 扫描 Inode Block，找 dip->type==0
          │    ├─ 清零 dinode，设置 type=T_FILE
          │    └─ log_write(该 Inode Block)
          ├─ ilock(ip)，设置元数据（如 nlink=1）
          ├─ iupdate(ip) → log_write(该 Inode Block)
          ├─ dirlink(dp, "x", ip->inum)
          │    └─ writei(dp, ...dirent...)
          │        └─ 修改父目录 Data Block → log_write(该块)
          └─ iunlockput(dp)，返回 ip
      └─ end_op() → 若 outstanding==0，commit()
```

**重要结果：**

- 磁盘中存在一个新的 `dinode`：`type=T_FILE`、`nlink` 已更新、`size=0`、`addrs[]` 尚无文件数据块。
- 父目录的数据块新增 `dirent {name="x", inum=新 inode 编号}`。
- 如果修改了 Inode Block 和目录 Data Block，就要登记日志；可能还涉及其他 inode 元数据更新。
- **`x` 的名字不保存在新文件的 inode 中，而是保存在根目录的 Data Block 中。**

### 阶段 C：`echo` 通过文件描述符 1 写入 `hi\n`

```text
用户态 echo
  └─ write(1, "hi\n", 3)
       └─ sys_write()
           └─ filewrite()
               └─ begin_op()
                   └─ ilock(ip)
                   └─ writei(ip, user_src=1, src, off=0, n=3)
                       ├─ bmap(ip, 0)
                       │    └─ addrs[0]==0 → balloc(dev)
                       │        ├─ 扫描 Bitmap → 找到空闲 Data Block（如 595）
                       │        ├─ Bitmap 对应 bit 从 0 变 1
                       │        ├─ log_write(Bitmap Block)
                       │        └─ bzero(新块) → log_write(新 Data Block)
                       ├─ bread(dev, 新块号)
                       ├─ either_copyin(bp->data, 1, src, 3)
                       │    └─ 写入 'h', 'i', '\n'
                       ├─ log_write(新 Data Block)
                       ├─ brelse(bp)
                       ├─ ip->size = 3
                       └─ iupdate(ip)
                            └─ 更新 size=3、addrs[0]=新块号
                                → log_write(Inode Block)
                   └─ iunlock(ip)
               └─ end_op() → 必要时 commit()
```

这次实际可能改动 **3 类不同磁盘块**：

| 类型 | 修改内容 | 为什么修改 |
|---|---|---|
| Bitmap Block | 新块对应 bit：0 → 1 | 宣告数据块已分配 |
| File Data Block | 开头 3 B 写入 `hi\n` | 保存实际文件内容 |
| Inode Block | 新文件 `size=3`，`addrs[0]=块号` | 让以后 `readi()` 能定位文件内容 |

这里 `bzero()` 和 `writei()` 可能都调用 `log_write(新 Data Block)`，但同一个事务里**只登记一次**：`log absorption`。如果新数据块尚未分配，日志可能先登记 Bitmap、数据块、Inode Block，具体顺序取决于程序路径。

### 阶段 D：事务如何写进磁盘 Log

假设阶段 C 恰好修改这三个原始块：

```text
Bitmap Block = 45
文件数据 Block = 595
Inode Block = 33
```

内存中的日志头最终为：

```c
log.lh.n = 3;
log.lh.block[0] = 45;
log.lh.block[1] = 595;
log.lh.block[2] = 33;
```

`end_op()` 确认没有其他尚未结束的文件系统操作，触发 `commit()`：

```text
1. write_log()
   bwrite 3       // 把 Block 45 最新内容复制到 Log Block 3
   bwrite 4       // 把 Block 595 最新内容复制到 Log Block 4
   bwrite 5       // 把 Block 33 最新内容复制到 Log Block 5

2. write_head()
   bwrite 2       // 写日志头 n=3, block=[45,595,33]，事务提交

3. install_trans()
   bwrite 45      // 将 Bitmap 修改安装到原位
   bwrite 595     // 将 hi\n 的数据安装到原位
   bwrite 33      // 将 inode 的 size/addrs[] 修改安装到原位

4. 清空日志头
   bwrite 2       // n=0，日志不再需要重放
```

这与截图中第二组 `3,4,5,2,45,595,33,2` 的**形式完全一致**。但块类型归属仍需运行时验证。

### 阶段 E：`close()`、Shell 与可能的附加事务

`echo` 执行完毕，相关文件描述符在进程退出时关闭（或显式 `close()`）。文件描述符与 inode 的引用会减少；**正常关闭不会自动意味着又修改文件内容**。在一些路径下 `iput()` 可能需要事务来安全处理最后引用及潜在 inode 回收。Shell 等其他操作也可能产生额外日志输出，因此屏幕上的 `bwrite` 数量不必与“创建一次、写入一次”严格一一对应。

### 阶段 F：再次读取文件 `x` 会发生什么？

```text
namei("x")
  → 从父目录 dirent 找到 x 的 inum
  → iget / ilock 得到 inode（size=3, addrs[0]=595）
  → readi(off=0, n=3)
  → bmap(ip, 0) 返回 595
  → bread(dev, 595)
  → either_copyout() 复制 "hi\n"
  → 用户得到文件内容
```

---

## 10. 把全部关键对象放在同一张图里

```text
磁盘（持久）                                      内存（临时）
┌────────────────────────────┐            ┌─────────────────────────────────┐
│ Superblock                 │ ──bread──> │ sb（全局 superblock）            │
│ Log Header + Log Data      │ <──commit──│ log.lh / log.outstanding         │
│ Inode Block：多个 dinode   │ ──bread──> │ Buffer Cache：整个 Inode Block   │
│                            │ ──ilock──> │ Inode Cache：单个 struct inode   │
│ Bitmap Block：每 bit/块    │ <──log──── │ Buffer Cache：整个 Bitmap Block  │
│ Directory Data Block       │ <──log──── │ Buffer Cache：目录 dirent 数据   │
│ File Data Block（hi\n）   │ <──log──── │ Buffer Cache：文件数据          │
└────────────────────────────┘            └─────────────────────────────────┘
```

注意箭头是概念性表示：`log_write()` 只是登记并固定缓存；真正把修改持久化到磁盘由后续的 `commit()` 完成。

## 11. 高频易错点（复习时可自测）

1. **Bitmap 是管 inode 的吗？** 不是；经典 xv6 Bitmap 管磁盘块。空闲 inode 通过磁盘 `dinode.type==0` 判断。
2. **Buffer Cache 只能缓存 Data Block？** 不是；也能缓存 Inode、Bitmap、Log 和 Superblock 等磁盘块。
3. **Buffer Cache 中的 dinode 就是 inode cache 吗？** 不是；`bcache.buf[]` 与 `itable.inode[]` 是两个不同的内核缓存。
4. **`bmap(ip,bn)` 的 `bn` 是物理磁盘块号？** 不是；是文件内部的逻辑块号。
5. **`ip->addrs[0]` 是文件的第 0 个字节？** 不是；是文件第 0 个逻辑数据块的**磁盘块号**。
6. **`refcnt` / `ip->ref` 是总访问次数？** 都不是；分别表示 Buffer 和内存 inode 的当前引用数。
7. **`brelse()` 会写磁盘？** 不会；它释放 Buffer。
8. **`log_write()` 会马上写 Log Block？** 不会；只登记块号、去重与 pin Buffer。
9. **一个 Block 改两次，Log 中要存两个版本？** 不需要；同事务只记录最终整块内容。
10. **`write_head()` 为什么放在 `write_log()` 后？** 因为有效磁盘 Header 是提交点，必须确保所有日志数据块已写好。
11. **截图中 `bwrite 2` 为什么一组出现两次？** 第一次写有效 Log Header 提交，第二次写 `n=0` 清除提交记录。
12. **`echo "hi" > x` 为什么既写目录块又写文件块？** 目录块登记名字 `x→inum`；文件块保存 `hi\n`；inode 保存文件大小和数据块地址。

## 12. 一句话主线

**`echo "hi" > x`：Shell 将标准输出重定向到文件 x → 若不存在则分配 dinode 并在目录 Data Block 新增名字映射 → `writei()` 用 `bmap/balloc` 取得数据块并修改 Bitmap、Data Block、Inode Block → `log_write()` 对这些块去重登记与 pin → 最后一个 `end_op()` 调用 `commit()`：先写 Log Data、再写 Log Header、再覆盖原始块、最后清 Header → 崩溃后可以根据日志完整恢复。**
