#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"
#include <stdlib.h>

#define MAX_MAP_LENGTH (1 << 21)

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);

  struct proc *p = myproc();
  addr = p->sz;

  // Prevent collisions with mappings
  for(int i = 0; i < MAX_MMAPS; ++i) {
    if(!p->mappings[i].is_mapped)
      continue;
    if(p->mappings[i].addr < addr + n)
      return -1;
  }

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    if(addr + n > TRAPFRAME)
      return -1;

    // Update free list if free space has changed
    struct free_segment *seg = p->free_list_head;
    while(seg) {
      if (seg->start <= PGROUNDUP(p->sz)) {
        seg->start = PGROUNDUP(p->sz + n);
        break;
      }
      seg = seg->next;
    }

    p->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_getmmapinfo(void)
{
  uint64 uaddr;
  argaddr(0, &uaddr);

  struct mmapinfo mmap_info = {0};
  struct proc *p = myproc();

  mmap_info.total_mmaps = p->total_mmaps;
  int ctr = 0;
  for(int i = 0; i < MAX_MMAPS; ++i) {
    if(!p->mappings[i].is_mapped)
      continue;
    mmap_info.addr[ctr] = (void *)p->mappings[i].addr;
    mmap_info.length[ctr] = p->mappings[i].length;
    mmap_info.n_loaded_pages[ctr] = p->mappings[i].shared->num_allocated;
    ++ctr;
  }

  either_copyout(1, uaddr, &mmap_info, sizeof(mmap_info));
  return 0;
}

static void
remove_segment(struct free_segment *seg, struct proc *p) {
  if(seg->prev) {
    seg->prev->next = seg->next;
    if(seg->next) seg->next->prev = seg->prev;
  }
  else {
    p->free_list_head = seg->next;
    if(seg->next) seg->next->prev = NULL;
  }

  seg->start = 0;
  seg->end = 0;
  seg->prev = NULL;
  seg->next = NULL;
}

static void
add_to_mappings(struct proc *p, uint64 addr, int length, int flags) {
  for (int i = 0; i < MAX_MMAPS; ++i) {
    if (!p->mappings[i].is_mapped) {

      struct underlying_mapping *s = kalloc();
      if(s == 0)
        panic("kalloc underlying_mapping");
      memset(s, 0, PGSIZE);

      s->ref_count = 1;
      s->num_allocated = 0;

      s->phys_pages = kalloc();
      if(s->phys_pages == 0)
        panic("kalloc underlying mapping");
      memset(s->phys_pages, 0, PGSIZE);

      for(int i = 0; i < NUM_PAGES; ++i)
        s->phys_pages->pages[i] = 0;

      p->mappings[i] = (struct mapping){
        .is_mapped = 1,
        .addr = addr,
        .length = length,
        .flags = flags,
        .shared = s
      };

      ++p->total_mmaps;
      return;
    }
  }
}

struct free_segment *create_segment(struct proc *p){
  for(int i = 0; i < MAX_MMAPS + 2; ++i){
    struct free_segment *seg = p->segment_pool->free_segments[i];
    if(!seg->start && !seg->end){
      return seg;
    }
  }
  return NULL;
}

uint64
sys_mmap(void)
{
  uint64 addr;
  int length;
  int flags;

  argaddr(0, &addr);
  argint(1, &length);
  argint(2, &flags);

  // Check process max maps
  struct proc *p = myproc();
  if(p->total_mmaps >= MAX_MMAPS) {
    return 0;
  }

  length = PGROUNDUP(length);

  // Check length
  if(length <= 0 || length > MAX_MAP_LENGTH) {
    return 0;
  }

  // Check flags
  if((flags & MAP_ANONYMOUS) == 0 ||
      (flags & MAP_SHARED) == 0) {
    return 0;
  }

  // Check valid address
  if(flags & MAP_FIXED &&
    (addr >= TRAPFRAME || addr < p->sz || addr % PGSIZE != 0)) {
    return 0;
  }

  // Try fitting mapping at suggested address
  struct free_segment *seg = p->free_list_head;
  struct free_segment *candidate_segment = NULL;
  struct free_segment *next_seg = NULL;
  while (seg) {
    next_seg = seg->next;
    // Save highest-address candidate segment
    if(candidate_segment == NULL && (length <= seg->end - seg->start)) {
      candidate_segment = seg;
    }

    // Suggested address works
    if(addr >= seg->start && addr + length <= seg->end) {
      uint64 map_start = addr;
      uint64 map_end = addr + length;
      if(map_start != seg->start && map_end != seg->end) {
        // Free segment is partitioned into two segments
        struct free_segment *new_seg = create_segment(p);
        if(!new_seg){
          return 0;
        }
        new_seg->start = map_end;
        new_seg->end = seg->end;
        seg->end = map_start;

        // new_seg at higher address, comes first in free list
        if(seg->prev) {
          seg->prev->next = new_seg;
        } else {
          p->free_list_head = new_seg;
        }
        new_seg->prev = seg->prev;
        new_seg->next = seg;
        seg->prev = new_seg;
      }
      else if(map_start == seg->start && map_end == seg->end) {
        // Free segment is entirely consumed, delete segment
        remove_segment(seg, p);
      }
      else{
        // Free segment is split
        if(map_start == seg->start)
          seg->start = map_end;
        else if(map_end == seg->end)
          seg->end = map_start;
      }

      // add to mappings
      add_to_mappings(p, addr, length, flags);
      return addr;
    } else if(seg->end < addr && candidate_segment) {
      break;
    }
    seg = next_seg;
  }
  // Didn't fit at suggested address
  if(flags & MAP_FIXED) {
    return 0;
  }
  
  // Use highest-address segment
  if (candidate_segment) {
    uint64 vaddr = candidate_segment->end - length;
    if (vaddr == candidate_segment->start) {
      // Entire segment consumed, delete segment
      remove_segment(candidate_segment, p);
    } else {
      candidate_segment->end = vaddr;
    }
    add_to_mappings(p, vaddr, length, flags);
    return vaddr;
  }
  return 0;
}

uint64
sys_munmap(void)
{
  // TODO: implement!
  uint64 addr;
  argaddr(0, &addr);

  struct proc *p = myproc();
  for (int i = 0; i < MAX_MMAPS; ++i) {
    struct mapping *map = &p->mappings[i];
    if (map->is_mapped) {
      if (map->addr == addr) {
        uint64 map_start = map->addr;
        uint64 map_end = map_start + map->length;

        free_mapping(p->pagetable, map);
        --p->total_mmaps;

        // Update free list
        struct free_segment *new_seg = create_segment(p);
        new_seg->start = map_start;
        new_seg->end = map_end;

        struct free_segment *high_seg = p->free_list_head;
        struct free_segment *low_seg = NULL;

        // Insert new free segment into free list
        if(high_seg->end <= map_start){
          new_seg->prev = NULL;
          new_seg->next = high_seg;
          p->free_list_head = new_seg;
          high_seg->prev = new_seg;
        } else {
          while(high_seg) {
            low_seg = high_seg->next;
            if(!low_seg || low_seg->end <= map_start){
              high_seg->next = new_seg;
              new_seg->prev = high_seg;
              new_seg->next = low_seg;
              if(low_seg) low_seg->prev = new_seg;
              break;
            }
            high_seg = high_seg->next;
          }
        }
        // Merge adjacent free segments
        high_seg = p->free_list_head;
        while(high_seg){
          low_seg = high_seg->next;
          if(!low_seg) break;
          if(low_seg->end == high_seg->start){
            high_seg->start = low_seg->start;
            remove_segment(low_seg, p);
            continue;
          }
          high_seg = high_seg->next;
        }
        return 0;
      }
    }
  }

  return -1;
}

