# COS417 Spring 2026 Assignment 4: Memory Mapping

<!-- markdown-toc start - Don't edit this section. Run M-x markdown-toc-refresh-toc -->
**Table of Contents**

- [COS417 Spring 2026 Assignment 4: Memory Mapping](#cos417-spring-2026-assignment-4-memory-mapping)
  - [Quick References](#quick-references)
  - [Introduction](#introduction)
  - [Objectives](#objectives)
  - [Adding Memory Maps to `xv6`'s Process Virtual Address Space](#adding-memory-maps-to-xv6s-process-virtual-address-space)
    - [Challenge 1: Avoiding collisions between `sbrk()` and `mmap()`](#challenge-1-avoiding-collisions-between-sbrk-and-mmap)
    - [Challenge 2: Lazily-Allocated Memory Mappings](#challenge-2-lazily-allocated-memory-mappings)
    - [Challenge 3: Lazy-Allocated Mappings Across Forks](#challenge-3-lazy-allocated-mappings-across-forks)
  - [Simplifying Assumptions](#simplifying-assumptions)
  - [The `mmap` System Call Interface](#the-mmap-system-call-interface)
  - [Task 1: Tracking Memory Mappings in the Kernel](#task-1-tracking-memory-mappings-in-the-kernel)
  - [Task 2: Implement The `getmmapinfo()` System Call](#task-2-implement-the-getmmapinfo-system-call)
  - [Task 3: Implement the `mmap()` System Call](#task-3-implement-the-mmap-system-call)
  - [Task 4: Implement the `munmap()` System Call](#task-4-implement-the-munmap-system-call)
  - [Task 5: Realizing Mappings on MMAP Faults](#task-5-realizing-mappings-on-mmap-faults)

<!-- markdown-toc end -->

## Quick References

1. [Linux's `mmap` System Call Manpage][linux-mmap-man]
1. [Xv6 memory layout (Section 3.6)][xv6-process-memory-layout]
1. [39-bit Page-Based Virtual-Memory System][riscv-isa-priv-manual-sv39]
1. [RISC-V trap codes (Table 16)][riscv-isa-priv-manual-scause]

## Introduction

In this project, we continue our exploration of `xv6`'s memory management
systems. You will implement a technique called "memory mapping", where programs
can create new "mapped" memory regions in their virtual address space.

Memory mapping is a very powerful technique common in many operating systems. It
allows a process to gain access to new chunks of memory (outside of the
linear heap, whose size is controlled with `sbrk()`). These memory regions can also
be shared with other processes, like forked children, to implement inter-process
communication (IPC). Finally, they can be used to make the contents of files
stored on the file system available to the program, simply by reading and
writing memory (called file-backed mappings). You can have a look at the [Linux
`mmap()` system call manpage][linux-mmap-man] to see what a full-fledged `mmap`
implementation can do.

In this assignment, we will implement a much simpler version of `mmap`. We don't
support many of the flags that you can pass in the Linux version of `mmap` and,
most notably, we don't implement file-backed memory mappings.

Nonetheless, in this assignment, you will implement a form of memory mappings
that can be shared between different processes, which is very useful for
exchanging data between processes. Along the way, you will learn about how the
virtual address space of `xv6`'s processes is organized. You will also re-use
concepts learned in the previous assignment, such as reference-counting for
kernel objects.

## Objectives

- Understand virtual memory, address spaces, and OS memory management techniques
- Learn how shared resources are handled in a process hierarchy
- Implement pagefault handlers for lazy-allocated memory regions
- Manipulate the `xv6` / RISC-V process page tables

## Adding Memory Maps to `xv6`'s Process Virtual Address Space

Currently, `xv6`'s memory layout looks roughly like this (see [the `xv6` manual,
Section 3.6][xv6-process-memory-layout], for a more detailed overview):

```
+----------------------------------+ 0x40_0000_0000 (MAXVA)
|           Trampoline             |
|           Trapframe              |
+----------------------------------+ 0x3f_ffff_e000 (TRAPFRAME)
|                                  |
|                                  |
|                                  |
|                                  |
|             Unused               |
|                                  |
|                                  |
|                                  |
|                                  |
+^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^+ p->sz
|              Heap                |
|              Stack               |
|              Data                |
|              Text                |
+----------------------------------+ 0x00_0000_0000
```

Looking at this diagram, we can see three distinct sections:

1. At the top of the virtual address space (defined as `MAXVA` in xv6, which is a
   software limit not a [RISC-V limit][riscv-isa-priv-manual-sv39],
   the kernel maps two special pages: a
   "trampoline" page, and a "trapframe" page. These pages are used by the `xv6`
   kernel to facilitate context switches between the kernel and the process. You
   do not need to understand these in detail, except that `xv6` will
   always map these pages to the top of the virtual address space, between
   `TRAPFRAME` and `MAXVA`.

2. At the bottom of the address space, we find the process' code and data.
   The process binary is generally loaded starting at address 0, which
   includes things like the program text or constant data. After these sections,
   follow the process' static data, stack, and finally heap sections.

   The heap does not have a fixed size: your program can use the
   `sbrk` system call to change the size of the heap, which modifies an internal
   variable in the Process Control Block (called `sz`), and modifies the
   process' page table to reflect this.

   Note that `xv6` performs lazy allocation: when a process runs `sbrk()`, it
   is possible for the value `p->sz` to be increased without actually allocating
   the physical memory or mapping it in the process page table.
   Only once the process tries to access these pages will a trap be triggered that
   causes the `usertrap()` handdler to run `vmfault()` and map in these pages.

3. Finally, the virtual address space contains a huge section of unused and
   unmapped pages.

In this assignment, you will add a system call to the operating system that can
map pages anywhere within this unused portion of the process's
address space:

```
+----------------------------------+ 0x40_0000_0000 (MAXVA)
|           Trampoline             |
|           Trapframe              |
+----------------------------------+ 0x3f_ffff_e000 (TRAMPOLINE)
|                                  |
|             Unused               |
|                                  |
+----------------------------------+ 0x20_dead_4000
|           Mapping #1             |
+----------------------------------+ 0x20_beef_9000
|             Unused               |
+----------------------------------+ 0x10_f00d_0000
|           Mapping #2             |
+----------------------------------+ 0x10_8bad_0000
|             Unused               |
+^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^+ p->sz
|              Heap                |
|              Stack               |
|              Data                |
|              Text                |
+----------------------------------+ 0x00_0000_0000
```

There are a few challenges that come with implementing the `mmap` syscall:

### Challenge 1: Avoiding collisions between `sbrk()` and `mmap()`

Without an `mmap()` syscall, `sbrk()` was able to grow the heap
as long as it stayed below the Trampoline and Trapframe pages.
That allows the process to use up to 256 TB of memory
--- significantly more than today's average computer has physical memory.

With `mmap()`, however, the area between the heap and Trapframe page can have
memory mappings scattered about. This means
when we creating new mappings, we must not overlap a mapping with the current
heap. The same is true when growing the heap: the heap must not expand into
existing memory mappings.

### Challenge 2: Lazily-Allocated Memory Mappings

A common use of `mmap()` in real operating systems is to allocate a
large amount of virtual memory that is not backed by physical memory. One
application of this "lazy allocation" approach is implementing sparse data
structures like hash tables or matrices which do not need physical pages
for their vast ranges of empty data. And, for an `mmap()` implementation
that supports file-backed mappings, lazy allocation allows you to only
load the parts of the file that the program actually accesses.

In this assignment, your `mmap` implementation should also lazily allocate the
underlying memory pages (referred to as _realizing_ a mapping). We call such
mappings "unrealized" before the program actually tries to access them.

This actually ties in to the previous challenge: to make sure the process break
(`p->sz`) does not collide with any mappings, this means that you have to not
only consider whether there currently is a page table entry for a mapping, but
whether they may be a lazy-allocated one in the future.

### Challenge 3: Lazy-Allocated Mappings Across Forks

Consider the following sequence of events:
1. Process A calls `mmap()` to create a new memory mapping.
2. Process A forks, creating child process B.
3. Process A writes to the memory mapping. This faults, because the mapping was
   lazily allocated. The kernel allocates the physical pages and maps them into
   process A's pagetable, allowing its write to proceed.
4. Process B reads from the mapping, which is configured to be shared across
   `fork()` calls. Process B should be able to read the exact page that A wrote
   to, despite not having the shared page mapped in its own page table yet.

The order of reads and writes and the order of processes does not matter:
the first process to access the page will allocate the page lazily while any other
processes that share the mapping should be able to access the same physical page
later. Therefore, you must ensure that the lazily allocated page mappings for `mmap`ed
memory regions are consistent across all processes that share this mapping.

Throughout the remainder of the assignment, we will provide you with some hints
for how you can implement `mmap` despite these challenges. But now is a good
time to think: can you come up with any data structure(s) in the
kernel that are able to track all of this information?

## Simplifying Assumptions

For this project, we are making a few simplifying assumptions:

1. We can have at most 64 mappings per process (see the `MAX_MMAPS` define in
   the `kernel/mmap.h` file). Your `mmap()` should fail
   (e.g., by returning a non-zero return code) when a process exceeds this
   limit.

2. You only need to support mappings of up to 2 MB in size (which translates to
   512 * 4 kB pages). So, any data structures you use to keep track of pages for
   a mapping just need to fit at most 512 entries.

   Your implementation is allowed, but not required, to support larger mappings.
   We also encourage you to think about data structures that can support such
   larger mappings.

## The `mmap` System Call Interface

Before you begin, we need to intoduce the basic `mmap`
interface that your programs will use to request new memory mappings in their
address space.

This is the `mmap` system call signature:

```c
void* mmap(void* addr, uint64 length, uint flags)
```

Arguments:

* `addr`: Depending on the flags (`MAP_FIXED`, more on that later), it could be
  a hint for what "virtual" address `mmap` should use for the mapping, or the
  "virtual" address that `mmap` MUST use for the mapping. A
  valid `addr` must be a multiple of page size and below `MAXVA`.

* `length`: The length of the mapping in bytes. *It must be greater than 0.*

* `flags`: The kind of memory mapping you are requesting. Flags can be
  **ORed** together (e.g., `MAP_SHARED | MAP_ANONYMOUS`).

  We have included definitions of the subset of flags you will implement
  in the `kernel/mmap.h`
  header file. If you look at the [man page for Linux's mmap][linux-mmap-man],
  there are many flags for various purposes. In your implementation, you only
  need to implement these flags:

  1. `MAP_ANONYMOUS`: represents memory that has no file associated with it.
     This flag must always be set for this assignment; if it is not, `mmap()`
     must return an error.

     When this flag is set, the mapping will be backed by lazily allocated
     physical memory. You must ensure that the physical pages are cleared
     (zeroed) before they are made accessible to the process.

  2. `MAP_SHARED`: this flag indicates that the mapping is shared between
     processes. When a process forks, its child will also inherit the mapping.
     However, all processes that share this mapping also share the same physical
     memory that backs it. This means all modifications to the memory by one
     process, should be viewable by all that share it. Notice that this is
     different to copy-on-write where a separate physical copy is made for
     processes when one of them writes to the shared mapping. This flag must
     also always be set in this assignment, or else return an error.

  3. `MAP_FIXED`: this flag informs us on what to do with the first argument
     of the `mmap` - the `addr` argument. When `MAP_FIXED` is passed in,
     this mapping **must** start at the passed-in (virtual) address. If the
     mapping cannot be placed at that address for any reason, return an error.
     If the flag is not passed in as an argument, the passed-in address should be
     treated as a suggestion for where the mapping should be placed - the
     kernel makes the final decision on where to place the mapping.

Return value:

* On success, `mmap` should return the virtual address of the start of the
  memory mapping.

* On failure, `mmap` should return `0`.

## Task 1: Tracking Memory Mappings in the Kernel

To start with the assignment, **add or modify data structures in `kernel/proc.h`
that can track the current memory mappings set up for a process**.
You can use definitions and types from the included `mmap.h` file for your data
structures. Keep in mind the requirements outlined by the `mmap` interface and
challenges described above.

In particular, consider that:

1. We limit the maximum number of mappings per process to 64 (`MAX_MMAPS` in
   `mmap.h`). This allows you to keep track of a fixed maximum size of mappings.

2. You can, but do not need to support mappings exceeding 2 MB (512 4kB
   pages). This allows you to keep track of a fixed maximum number of physical
   pages in a memory mapped region.

   If you don't support mappings larger than 2 MB, your mmap must return an
   error for such a request. If your mmap succeeds for mappings larger than 2
   MB, then those mappings must end up accessible after completing Task 5.

3. Memory mappings can be shared across multiple processes through `fork()` calls.

   A memory mapping must be deallocated and freed only when the last remaining
   process that can access it has exited.

   All processes accessing a memory mapping with the `MAP_SHARED` parameter
   should see the same contents, and changes to the mapping by one process must
   be visible to all other processes, regardless of when and whether any given
   process initially reads from or writes to a mapping.

4. A memory mapping should lazily allocate pages.

   For instance, you could allocate a physical page only at the time
   when the first process **reads from or writes to** a given page with a
   mapping.

   Another approach could be that you initialize the mapping such that all pages
   initially reference a shared, zeored ("null") page, and allocate a physical
   page only when the first process writes to them. But beware: when using this
   approach, it may be much more complex to implement `MAP_SHARED` semantics
   correctly.

   Either way, you should not allocate physical pages for a mapping before a
   process wants to read from or write to them. This means requesting multiple
   mappings that in total exceed the system's physical memory is possible and
   should be allowed, so long as the physical pages backing them never surpasses
   the system limit.

   Hint: similar to copy-on-write, you may handle this using reference counting,
   but be sure not to reference individual physical pages.

Make sure that you properly initialize all new fields added to the Process
Control Block (PCB). The two initialization functions relevant here are
`allocproc()` and `kfork()`. Be sure that you clean up any necessary resources
in `kexit()` and/or `freeproc()`.

## Task 2: Implement The `getmmapinfo()` System Call

The `getmmapinfo()` system call provides information about the current memory
mappings of a process.

```c
void getmmapinfo(struct mmapinfo*);
```

It accepts a pointer, which it fills with the following data structure (already
defined in the `kernel/mmap.h` file):

```c
struct mmapinfo {
    uint64 total_mmaps;               // Total number of mmap regions
    void* addr[MAX_MMAPS];            // Starting address of each mapping
    uint64 length[MAX_MMAPS];         // Size of each mapping
    uint64 n_loaded_pages[MAX_MMAPS]; // Number of pages physically realized for
                                      // all mappings
};
```

When returning the length of the mapping, be sure to return its actual
length. You can only control process memory access on a per-page granularity,
and the `length` value returned for each mapping in `getmmapinfo` should reflect
that.

## Task 3: Implement the `mmap()` System Call

Next, **implement the `mmap()` system call**. You should implement it starting
with the scaffolding for this system call provided in `kernel/sysproc.c`.

Because memory mappings are lazily allocated, we will not need to modify the
process page table just yet. For now, we need to track the
metadata of the unrealized lazy mappings.
**After completing this task, it is expected for a process to fault if
it tries to access a new mapping**.

Make sure that you handle the following non-exhaustive list of edge-cases:

1. Your `mmap()` system call fails when the user specifies incorrect flags,
   as per the interface description above.

2. Your system call should fail if a process tries to add more than
   `MAX_MMAPS` mappings.

3. Forks of your process should inherit their parents mappings.

4. Mappings cannot overlap with each other. When a user uses `MAP_FIXED` and
   specifies an address that would collide with an existing mapping for this
   process, `mmap` must return an error.

5. Mappings cannot collide with existing, (potentially lazily) allocated
   process memory, such as the process' heap or the Trampoline and Trapframe
   pages. Requests with `MAP_FIXED` and an address that would collide with these
   regions must fail.

6. The process cannot grow its virtual memory "break" (using, e.g., the `sbrk()`
   system call) into an existing mapping.

When not supplying `MAP_FIXED`, your `mmap` implementation should try to find a
virtual address suitable for this mapping if and only if the "suggested address"
supplied to the `mmap` system call cannot be used. In this case, you should
avoid choosing addresses that will limit the amount of virtual memory
that a heap can grow into (i.e., try not to choose lower
virtual addresses if you can).

After this task, using the `mmaptests` program, you should be able to pass the
`task3` group of test cases, which you can run by executing `mmaptests -g task3`
in the `xv6` shell. It is possible that you get an error saying `FAILED -- lost
some free pages XXXXX (out of XXXXX)`. That is OK for now---the next task
considers cleaning up resources and ensuring that you do not leak
memory. You can use `mmaptests <test name>` to debug individual
test cases.

## Task 4: Implement the `munmap()` System Call


Th `munmap()` function signature looks as follows:

```c
int munmap(void* addr)
```

If `addr` refers to a valid memory mapping for the current process, then this
call should remove the entire mapping.

`addr` must match the exact address returned by `mmap` for a currently valid (not
yet unmapped) mapping; any other address (e.g., one inside a mapping) should be
rejected.

Make sure that you free and reclaim any data structures and resources related to
a mapping once no remaining processes can access this memory mapping. If another,
process still references the mapping, do not destroy the mapping, simply unmap it
for the current process.

After this task, you should be able to pass all tests of the `task4` test
group. This set of tests includes all tests from the `task3` group, as well.
You should see an `ALL TESTS PASSED` statement with no "lost free pages" messages
if you implemented `munmap()` correctly.

## Task 5: Realizing Mappings on MMAP Faults

Finally, you should **implement the logic required for programs to actually be
able to access their memory mappings**. This will look quite similar to the
implementation of the `cowfault()` handler of the previous assignment, with a few
differences.

When a process accesses a mapped, but unrealized page of memory, you must allocate
only that particular page. You must
also make sure that all processes sharing this mapping are using the *same*
physical pages, such that contents written to the memory by one process must be
visible to all other processes sharing the mapping.

You will need to modify functions that handle a process exiting so that physical pages
for a mapping are freed when no other processes reference the mapping.

Once you finish this task, you should be able to pass all `mmap` specific tests by
running `mmaptests -g task5` or the equivalent command `mmaptests`.
Additionally, you should now run the
full `usertests` test suite without arguments to run and pass all regression tests for
other OS functionality not related to `mmap`.

## Submission/Deliverables

You must do three things to turn in this assignment.

1) Fill in your name, netid, and AI disclosure statement at the top of the
`kernel/vm.c` file.
2) Submit the `xv6` directory (which should contain all of your code changes) using the `handin` command.
Usage: `handin mmap [path to the xv6 directory]`
3) Submit a `git diff` file with your code changes to Gradescope. This file should be created automatically by the `handin` script; it will print out the absolute path at which you can find this file.
Upload this file to Gradescope under the A4 MMAP assignment by first copying
the file to your local computer:

```sh
scp <netid>@courselab.cs.princeton.edu:<path printed by handin> .
```

The period at the end of this command means copy the file and store it in the
current directory. You can change the destination period to whatever path you wish.

**Note that the `handin` command from Step 2 of the above process will
be used as the official, archival version of your handed-in
assignment! It is *most* important that this step completes
successfully.**

[linux-mmap-man]: https://man7.org/linux/man-pages/man2/mmap.2.html
[xv6-process-memory-layout]: https://pdos.csail.mit.edu/6.828/2025/xv6/book-riscv-rev5.pdf#page=38
[riscv-isa-priv-manual-sv39]: https://riscv.github.io/riscv-isa-manual/snapshot/privileged/#sv39
[riscv-isa-priv-manual-scause]: https://riscv.github.io/riscv-isa-manual/snapshot/privileged/#scause
