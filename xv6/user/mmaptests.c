#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"
#include "kernel/mmap.h"

// Program name, set by main. Used for exec calls to this same binary:
char *progname;

// Store these statically, as they're large and we want to avoid a stack
// overflow:
struct mmapinfo inf1, inf2;

struct mmapargs {
  void* addr;
  uint64 length;
  uint flags;
};

// Simple bubblesort over mmapinfo, to compare structs
void
sort_mmapinfo(struct mmapinfo *inf)
{
  void* tmp_addr;
  uint64 i, j, tmp_length, tmp_n_loaded_pages;

  if (inf->total_mmaps > MAX_MMAPS) {
    printf("inf->total_mmaps > MAX_MMAPS!\n");
    exit(1);
  }

  for (i = 0; i < inf->total_mmaps; i++) {
    for (j = 0; j < inf->total_mmaps - i - 1; j++) {
      if (inf->addr[j] > inf->addr[j + 1]) {
        tmp_addr = inf->addr[j];
        tmp_length = inf->length[j];
        tmp_n_loaded_pages = inf->n_loaded_pages[j];
        inf->addr[j] = inf->addr[j + 1];
        inf->length[j] = inf->length[j + 1];
        inf->n_loaded_pages[j] = inf->n_loaded_pages[j + 1];
        inf->addr[j + 1] = tmp_addr;
        inf->length[j + 1] = tmp_length;
        inf->n_loaded_pages[j + 1] = tmp_n_loaded_pages;
      }
    }
  }
}

int
mmapinfocmp(struct mmapinfo *a, struct mmapinfo *b)
{
  sort_mmapinfo(a);
  sort_mmapinfo(b);

  if (a->total_mmaps != b->total_mmaps) {
    return 1;
  }

  for (uint64 i = 0; i < a->total_mmaps; i++) {
    if (a->addr[i] != b->addr[i]
        || a->length[i] != b->length[i]
        || a->n_loaded_pages[i] != b->n_loaded_pages[i]) {
      return 1;
    }
  }

  return 0;
}

void
print_mmapinfo(struct mmapinfo *inf)
{
  if (inf->total_mmaps > MAX_MMAPS) {
    printf("inf->total_mmaps > MAX_MMAPS!\n");
    exit(1);
  }

  printf("struct mmapinfo {\n  total_mmaps = %lu,\n  mappings = [\n", inf->total_mmaps);
  for (uint64 i = 0; i < inf->total_mmaps; i++) {
    printf("    { addr = %p, len = %lu, n_loaded_pages = %lu },\n", inf->addr[i], inf->length[i], inf->n_loaded_pages[i]);
  }
  printf("  ]\n}\n");
}

// Test that mmap rejects invalid flags:
void
mmap_illegal_flags(char *s)
{
  void *addr;

  // Mappings must have MAP_SHARED:
  addr = mmap(0, PGSIZE, MAP_ANONYMOUS);
  if (addr) {
    printf("%s: mmap should fail when not passing MAP_SHARED\n", s);
    exit(1);
  }

  // Mappings must have MAP_ANONYMOUS:
  addr = mmap(0, PGSIZE, MAP_SHARED);
  if (addr) {
    printf("%s: mmap should fail when not passing MAP_ANONYMOUS\n", s);
    exit(1);
  }

  // Mappings must fail with no flags:
  addr = mmap(0, PGSIZE, 0);
  if (addr) {
    printf("%s: mmap should fail when passing flags = 0\n", s);
    exit(1);
  }

  // Mappings must fail with unknown flags:
  addr = mmap(0, PGSIZE, ~((uint) 0));
  if (addr) {
    printf("%s: mmap should fail when encountering unknown flags\n", s);
    exit(1);
  }
}

// Make sure that mmap fails on unaligned addresses:
void
mmap_unaligned(char *s)
{
  void *addr;

  addr = mmap((void*) 0x70000001, PGSIZE, MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED);
  if (addr) {
    printf("%s: mmap must fail on unaligned allocations\n", s);
    exit(1);
  }

  addr = mmap((void*) 0x70000FFF, PGSIZE, MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED);
  if (addr) {
    printf("%s: mmap must fail on unaligned allocations\n", s);
    exit(1);
  }

  addr = mmap((void*) 0x70000800, PGSIZE, MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED);
  if (addr) {
    printf("%s: mmap must fail on unaligned allocations\n", s);
    exit(1);
  }

  // Check that mmap works for a properly aligned address here:
  addr = mmap((void*) 0x70000000, PGSIZE * 2, MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED);
  if (!addr) {
    printf("%s: mmap fails on properly aligned address\n", s);
    exit(1);
  }

  // Check that mmap for an unaligned address works, as long as MAP_FIXED is not passed:
  addr = mmap((void*) 0x70000800, PGSIZE, MAP_ANONYMOUS | MAP_SHARED);
  if (!addr) {
    printf("%s: mmap not fail on unaligned address requests when MAP_FIXED is not set\n", s);
    exit(1);
  }
}

// Make sure that the kernel chooses addresses that are not too close
// to the process break, when MAP_FIXED is not passed (we assume that
// any address above MAXVA / 2 is fine):
void
mmap_not_fixed_addr_selection(char *s)
{
  void *addr;

  addr = mmap(0, PGSIZE * 2, MAP_ANONYMOUS | MAP_SHARED);
  if (!addr) {
    printf("%s: mmap failed\n", s);
    exit(1);
  }

  if (addr < (void*) (MAXVA / 2)) {
    printf("%s: mmap chose address lower than %p (returned address %p)", s, (void*) (MAXVA / 2), addr);
    exit(1);
  }
}

// This test ensures that mmap refuses zero-length mappings:
void
mmap_zero_length(char *s)
{
  void *addr;
  uint64 brk = (uint64) sbrk(0);
  struct mmapargs testcases[4] = {
    {
      .addr = (void*) TRAPFRAME - PGSIZE,
      .length = 0,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) PGROUNDUP(brk),
      .length = 0,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = 0,
      .length = 0,
      .flags = MAP_ANONYMOUS | MAP_SHARED, // not fixed!
    },
    {
      .addr = (void*) TRAPFRAME - PGSIZE,
      .length = 0,
      .flags = MAP_ANONYMOUS | MAP_SHARED, // not fixed!
    },
  };

  for (uint64 i = 0; i < sizeof(testcases) / sizeof(testcases[0]); i++) {
    addr = mmap(testcases[i].addr, testcases[i].length, testcases[i].flags);
    getmmapinfo(&inf1);
    if (addr || inf1.total_mmaps != 0) {
      printf("%s: mmap succeeded with length = 0 (test case %lu)\n", s, i);
      exit(1);
    }
  }
}

// Check that mmap either refuses length >= 2 MB or, if it accepts
// that parameter, mappings over 2 MB are actually accessible:
void
mmap_larger_than_2mb(char *s)
{
  void *addr;

  addr = mmap(0, 2 * 1024 * 1024 + PGSIZE, MAP_SHARED | MAP_ANONYMOUS);
  if (!addr) {
    // It's OK for an implementation not to support mappings over 2MB
    return;
  }

  // THis implementation does support mappings over 2MB, make sure
  // they're accessible:
  ((char *) addr)[0] = 'x';
  ((char *) addr)[1024 * 1024] = 'v';
  ((char *) addr)[2 * 1024 * 1024 + 1] = '6';

  if (munmap(addr) != 0) {
    printf("%s: munmap failed\n", s);
    exit(1);
  }
}

// This test ensures that a mapping with length % PGSIZE != 0 gets its length
// rounded up to a full page size:
void
mmap_length_roundup_base(char *s, int realize, int unmap)
{
  void* addrs[3];
  uint64 brk = (uint64) sbrk(0);
  struct mmapargs testcases[3] = {
    {
      .addr = (void*) TRAPFRAME - PGSIZE,
      .length = 1,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) PGROUNDUP(brk),
      .length = PGSIZE - 1,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = 0,
      .length = (PGSIZE * 8) + (PGSIZE / 2),
      .flags = MAP_ANONYMOUS | MAP_SHARED, // not fixed!
    }
  };

  // Try to map all test cases concurrently.
  for (uint64 i = 0; i < sizeof(testcases) / sizeof(testcases[0]); i++) {
    addrs[i] = mmap(testcases[i].addr, testcases[i].length, testcases[i].flags);
    if (!addrs[i] || ((testcases[i].flags & MAP_FIXED) && addrs[i] != testcases[i].addr)) {
      printf("%s: mmap failed (test case %lu)\n", s, i);
      exit(1);
    }
    if (realize) {
      ((char*) addrs[i])[42] = '!';
    }
  }

  getmmapinfo(&inf1);
  if (inf1.total_mmaps != sizeof(testcases) / sizeof(testcases[0])) {
    printf("%s: total_mmaps != len(testcases)\n", s);
    exit(1);
  }

  // Check if the length has been rounded up correctly:
  for (uint64 i = 0; i < inf1.total_mmaps; i++) {
    uint64 testcase;
    for (testcase = 0; testcase < sizeof(testcases) / sizeof(testcases[0]); testcase++) {
      // We filter out illegal mappings with `addr == 0` avoid double-using
      // `inf` entries. After we've found one, we set its addr[i] to 0 below.
      if (inf1.addr[i] != 0 && inf1.addr[i] == addrs[testcase]) {
        // inf[i] == testcases[testcase] == addrs[testcase]
        break;
      }
    }

    if (testcase == sizeof(testcases) / sizeof(testcases[0])) {
      printf("%s: getmmapinfo missing testcase addr!\n", s);
      exit(1);
    }

    if (inf1.length[i] != PGROUNDUP(testcases[testcase].length)) {
      printf(
        "%s: length for test case %lu not corrected rounded up: req %lu, returned %lu, expected %lu!\n",
        s,
        testcase,
        testcases[testcase].length,
        inf1.length[i],
        PGROUNDUP(testcases[testcase].length));
      exit(1);
    }

    // Make this testcase addr invalid (to prevent always matching in the same
    // testcase):
    inf1.addr[i] = 0;
  }

  // Optionally, unmap the mappings:
  if (unmap) {
    for (uint64 testcase = 0; testcase < sizeof(testcases) / sizeof(testcases[0]); testcase++) {
      if (munmap(addrs[testcase]) != 0) {
        printf("%s: munmap failed for test case %lu (addr %p)\n", s, testcase, addrs[testcase]);
        exit(1);
      }
    }
  }
}

void
mmap_length_roundup_nounmap(char *s)
{
  return mmap_length_roundup_base(s, 0, 0);
}

void
mmap_length_roundup_unmap(char *s)
{
  return mmap_length_roundup_base(s, 0, 1);
}

void
mmap_length_roundup_realized(char *s)
{
  return mmap_length_roundup_base(s, 0, 1);
}

// This test ensures that a MAP_FIXED mmap fails, when it is requested at an
// address lower than the process break:
void
mmap_before_brk_fail(char *s)
{
  void *addr;
  uint64 req_addr = (uint64) sbrk(0);

  // Want to mmap a region that _overlaps_ with the process break. If the
  // process break happens to be page-aligned, then we need to subtract a
  // page. Otherwise, round down.
  if (req_addr % PGSIZE == 0) {
    req_addr -= PGSIZE;
  } else {
    req_addr = PGROUNDDOWN(req_addr);
  }

  addr = mmap((void*) req_addr, PGSIZE, MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED);
  getmmapinfo(&inf1);
  if (addr != 0 || inf1.total_mmaps != 0) {
    printf("%s: mmap succeeded for address < p->sz\n", s);
    exit(1);
  }
}

// This test ensures taht a MAP_FIXED mmap fails when it is requested
// for addresses that overlap with TRAPFRAME, TRAMPOLINE, or are
// exceeding MAXVA:
void
mmap_high_mappings_collision(char *s)
{
  void *addr;
  struct mmapinfo inf;
  struct mmapargs testcases[10] = {
    {
      .addr = (void*) TRAPFRAME,
      .length = PGSIZE,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) TRAMPOLINE,
      .length = PGSIZE,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) TRAPFRAME,
      .length = PGSIZE * 2,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) TRAPFRAME - PGSIZE,
      .length = PGSIZE * 2,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) TRAPFRAME - PGSIZE,
      .length = PGSIZE * 3,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) TRAPFRAME - PGSIZE,
      .length = PGSIZE * 5,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) TRAMPOLINE,
      .length = PGSIZE * 2,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) MAXVA,
      .length = PGSIZE,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) MAXVA + PGSIZE,
      .length = PGSIZE,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) PGROUNDDOWN(~((uint64) 0)),
      .length = PGSIZE,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
  };

  for (uint64 i = 0; i < sizeof(testcases) / sizeof(testcases[0]); i++) {
    addr = mmap(testcases[i].addr, testcases[i].length, testcases[i].flags);
    getmmapinfo(&inf);
    if (addr || inf.total_mmaps != 0) {
      printf("%s: mmap suceeded with illegal addr = %p and len = %lu (test case %lu)\n", s, testcases[i].addr, testcases[i].length, i);
      exit(1);
    }
  }
}

// Ensure that we can map exactly MAX_MMAPS mappings, and not one more:
void
mmap_exceed_max_mmaps_base(char *s, int unmap)
{
  void *addrs[MAX_MMAPS];
  for (uint64 i = 0; i < MAX_MMAPS; i++) {
    addrs[i] = mmap(0, PGSIZE, MAP_ANONYMOUS | MAP_SHARED);
    if (!addrs[i]) {
      printf("%s: mmap no %lu failed\n", s, i);
      exit(1);
    }
  }

  // Next mmap must fail:
  void *exceeding_addr = mmap(0, PGSIZE, MAP_ANONYMOUS | MAP_SHARED);
  if (exceeding_addr) {
    printf("%s: mmap returned success for (MAX_MMAP + 1)th (%u) mapping\n", s, MAX_MMAPS + 1);
    exit(1);
  }

  if (unmap) {
    // Try unmapping a mapping in the middle, and then try mapping one again:
    if (munmap(addrs[4]) != 0) {
      printf("%s: munmap no 4 failed\n", s);
      exit(1);
    }

    addrs[4] = mmap(0, PGSIZE, MAP_ANONYMOUS | MAP_SHARED);
    if (!addrs[4]) {
      printf("%s: mmap failed to reuse slot 4\n", s);
      exit(1);
    }

    for (uint64 i = 0; i < MAX_MMAPS; i++) {
      if (munmap(addrs[i]) != 0) {
        printf("%s: munmap no %lu failed\n", s, i);
        exit(1);
      }
    }
  }
}

void
mmap_exceed_max_mmaps_nounmap(char *s)
{
  return mmap_exceed_max_mmaps_base(s, 0);
}

void
mmap_exceed_max_mmaps_unmap(char *s)
{
  return mmap_exceed_max_mmaps_base(s, 1);
}

void
mmap_brk_into_mapping_fail_base(char *s, int realize, int unmap)
{
  void *addr, *res, *brk, *req_addr;

  brk = sbrk(0);
  req_addr = (void*) PGROUNDUP((uint64) brk);

  addr = mmap(req_addr, PGSIZE, MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED);
  if (!addr || addr != req_addr) {
    printf("%s: mmap failed\n", s);
    exit(1);
  }

  if (realize) {
    ((char*) addr)[42] = '!';
  }

  // Try to move the process break into the mapping:
  res = sbrk(PGSIZE + 1);
  if (res != (void*) ~((uint64) 0)) {
    printf("%s: process break (originally %p) moved into mapping at %p through sbrk(PGSIZE + 1) = %p!\n", s, brk, addr, res);
    exit(1);
  }

  if (!unmap) {
    return;
  }


  if (munmap(addr) != 0) {
    printf("%s: munmap failed\n", s);
    exit(1);
  }

  // Try to move the process break again, this time with the mapping removed:
  res = sbrk(PGSIZE + 1);
  if (res == ((void*) -1)) {
    printf("%s: process break failed to move, despite munmap!\n", s);
    exit(1);
  }
}

void
mmap_brk_into_mapping_fail_unrealized_nounmap(char *s)
{
  return mmap_brk_into_mapping_fail_base(s, 0, 0);
}

void
mmap_brk_into_mapping_fail_unrealized_unmap(char *s)
{
  return mmap_brk_into_mapping_fail_base(s, 0, 1);
}

void
mmap_brk_into_mapping_fail_realized(char *s)
{
  return mmap_brk_into_mapping_fail_base(s, 1, 1);
}

// Test that mmaps themselves collide (both for unrealized and realized
// mappings), but only when they actually overlap. This also tests the auto
// address fallback for when `MAP_FIXED` is not provided:
void
mmap_collisions_base(char *s, int realized, int unmap)
{
  void *addr;

  struct mmapargs validmaps[2] = {
    {
      .addr = (void*) (TRAPFRAME - 8 * PGSIZE),
      .length = PGSIZE * 3,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) (TRAPFRAME - 3 * PGSIZE),
      .length = PGSIZE * 2,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
  };

  struct mmapargs failmaps[8] = {
    {
      .addr = (void*) (TRAPFRAME - 8 * PGSIZE),
      .length = PGSIZE * 8,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) (TRAPFRAME - 8 * PGSIZE),
      .length = PGSIZE,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) (TRAPFRAME - 7 * PGSIZE),
      .length = PGSIZE,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) (TRAPFRAME - 7 * PGSIZE),
      .length = PGSIZE * 5,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) (TRAPFRAME - 3 * PGSIZE),
      .length = PGSIZE * 2,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) (TRAPFRAME - 2 * PGSIZE),
      .length = PGSIZE * 2,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) (TRAPFRAME - 2 * PGSIZE),
      .length = PGSIZE,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) (TRAPFRAME - 2 * PGSIZE),
      .length = PGSIZE * 5,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
  };

  struct mmapargs goodmaps[4] = {
    {
      .addr = (void*) (TRAPFRAME - 11 * PGSIZE),
      .length = PGSIZE * 3,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) (TRAPFRAME - 14 * PGSIZE),
      .length = PGSIZE * 3,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) (TRAPFRAME - 5 * PGSIZE),
      .length = PGSIZE * 2,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
    {
      .addr = (void*) (TRAPFRAME - 1 * PGSIZE),
      .length = PGSIZE,
      .flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED,
    },
  };

  // Setup the basic valid mappings:
  for (uint64 i = 0; i < sizeof(validmaps) / sizeof(validmaps[0]); i++) {
    addr = mmap(validmaps[i].addr, validmaps[i].length, validmaps[i].flags);
    if (!addr || addr != validmaps[i].addr) {
      printf("%s: mmap validmaps[%lu] failed\n", s, i);
      exit(1);
    }

    if (realized) {
      for (uint64 j = 42; j < PGROUNDUP(validmaps[i].length); j += PGSIZE) {
        ((char*) addr)[j] = '!';
      }
    }
  }

  // Try to map the failmaps:
  for (uint64 i = 0; i < sizeof(failmaps) / sizeof(failmaps[0]); i++) {
    addr = mmap(failmaps[i].addr, failmaps[i].length, failmaps[i].flags);
    if (addr) {
      printf("%s: mmap failmaps[%lu] succeeded, but must fail!\n", s, i);
      exit(1);
    }
  }

  // If we're unmapping, then try to map the failmaps, but this time without
  // MAP_FIXED (which should always work for the lengths provided):
  if (unmap) {
    for (uint64 i = 0; i < sizeof(failmaps) / sizeof(failmaps[0]); i++) {
      addr = mmap(failmaps[i].addr, failmaps[i].length, failmaps[i].flags & ~MAP_FIXED);
      if (!addr) {
        printf("%s: mmap failmaps[%lu] without MAP_FIXED failed\n", s, i);
        exit(1);
      }

      if (realized) {
        for (uint64 j = 42; j < PGROUNDUP(failmaps[i].length); j += PGSIZE) {
          ((char*) addr)[j] = '!';
        }
      }

      if (munmap(addr) != 0) {
        printf("%s: munmap failmaps[%lu] without MAP_FIXED failed\n", s, i);
        exit(1);
      }
    }
  }

  // Map the goodmaps:
  for (uint64 i = 0; i < sizeof(goodmaps) / sizeof(goodmaps[0]); i++) {
    addr = mmap(goodmaps[i].addr, goodmaps[i].length, goodmaps[i].flags);
    if (!addr || addr != goodmaps[i].addr) {
      printf("%s: mmap goodmaps[%lu] failed\n", s, i);
      exit(1);
    }

    if (realized) {
      for (uint64 j = 42; j < PGROUNDUP(goodmaps[i].length); j += PGSIZE) {
        ((char*) addr)[j] = '!';
      }
    }

    if (unmap) {
      if (munmap(addr) != 0) {
        printf("%s: munmap goodmaps[%lu] failed\n", s, i);
        exit(1);
      }
    }
  }


  // Unmap the valid mappings (they're all MAP_FIXED):
  if (unmap) {
    for (uint64 i = 0; i < sizeof(validmaps) / sizeof(validmaps[0]); i++) {
      if (munmap(validmaps[i].addr) != 0) {
        printf("%s: munmap validmaps[%lu] failed\n", s, i);
        exit(1);
      }
    }
  }
}

void
mmap_collisions_nounmap(char *s)
{
  return mmap_collisions_base(s, 0, 0);
}

void
mmap_collisions_unmap(char *s)
{
  return mmap_collisions_base(s, 0, 1);
}

void
mmap_collisions_realized(char *s)
{
  return mmap_collisions_base(s, 1, 1);
}

// Ensure that mmap uses the suggested address even when MAP_FIXED is not
// present, when there's no collision:
void
mmap_respects_addr_hint(char *s)
{
  void *addr;

  // Check that mmap works for a properly aligned address here:
  addr = mmap((void*) 0x70000000, PGSIZE, MAP_ANONYMOUS | MAP_SHARED);
  if (!addr) {
    printf("%s: mmap fails on properly aligned address\n", s);
    exit(1);
  } else if (addr != (void*) 0x70000000) {
    printf("%s: mmap ignored address hint\n", s);
    exit(1);
  }
}

// Ensure that unmap doesn't work for illegal addresses that haven't been
// mapped:
void
munmap_invalid_address(char *s)
{
  void* testaddrs[8] = {
    (void*) 0x0,
    (void*) ~((uint64) 0),
    (void*) MAXVA,
    (void*) TRAPFRAME,
    (void*) TRAMPOLINE,
    (void*) 0xDEADBEEF,
    (void*) PGROUNDDOWN(0xDEADBEEF),
  };

  for (uint64 i = 0; i < sizeof(testaddrs) / sizeof(testaddrs[0]); i++) {
    int res;
    if ((res = munmap(testaddrs[i])) != -1) {
      printf("%s: munmap didn't return -1 for invalid address %p\n", s, testaddrs[i]);
      exit(1);
    }
  }

  // Now, add a mapping and make sure munmap still fails:
  void* addr = mmap(0, PGSIZE * 4, MAP_ANONYMOUS | MAP_SHARED);
  if (!addr) {
    printf("%s: mmap failed\n", s);
    exit(1);
  }

  for (uint64 i = 0; i < sizeof(testaddrs) / sizeof(testaddrs[0]); i++) {
    int res;
    if ((res = munmap(testaddrs[i])) != -1) {
      printf("%s: munmap didn't return -1 for invalid address %p\n", s, testaddrs[i]);
      exit(1);
    }
  }
}

// Make sure that munmap only accepts the exact start of a mapping, not an
// offset in the mapping:
void
munmap_reject_offset(char *s)
{
  void *addr;

  addr = mmap(0, PGSIZE * 3, MAP_SHARED | MAP_ANONYMOUS);
  if (!addr) {
    printf("%s: mmap failed\n", s);
    exit(1);
  }

  if (munmap(addr + 1) == 0 || munmap(addr + PGSIZE) == 0) {
    printf("%s: munmap must not succeed for offset in mapping\n", s);
    exit(1);
  }

  if (munmap(addr + PGSIZE * 3) == 0) {
    printf("%s: munmap succeeded for address outside of mapping\n", s);
    exit(1);
  }

  if (munmap(addr) != 0) {
    printf("%s: munmap failed\n", s);
    exit(1);
  }
}

// Test that mmaps are inherited in forks:
void
mmap_fork_inheritance_base(char *s, int realized, int unmap)
{
  void *addr1, *addr2, *addr3;
  int pid, wstatus;

  addr1 = mmap(0, PGSIZE * 3, MAP_SHARED | MAP_ANONYMOUS);
  if (!addr1) {
    printf("%s: mmap #1 failed\n", s);
    exit(1);
  }

  getmmapinfo(&inf1);
  if (inf1.total_mmaps != 1
      || inf1.addr[0] != addr1
      || inf1.length[0] != PGSIZE * 3
      || inf1.n_loaded_pages[0] != 0) {
    printf("%s: getmmapinfo #1 incorrect\n", s);
    print_mmapinfo(&inf1);
    exit(1);
  }

  pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  } else if (pid != 0) {
    // Parent!
    wait(&wstatus);
    if (wstatus != 0) {
      printf("%s: child exited with non-zero status = %d\n", s, wstatus);
      exit(1);
    }
  } else {
    // Child!
    getmmapinfo(&inf2);
    if (mmapinfocmp(&inf1, &inf2) != 0) {
      printf("%s: mmapinfo differs for child\n", s);
      print_mmapinfo(&inf1);
      print_mmapinfo(&inf2);
      exit(1);
    }

    if (realized) {
      // Try accessing the mapping in the child:
      ((char*) addr1)[42] = 'F';
      getmmapinfo(&inf2);
      if (inf2.total_mmaps != 1
          || inf2.addr[0] != addr1
          || inf2.length[0] != PGSIZE * 3
          || inf2.n_loaded_pages[0] != 1) {
        printf("%s: getmmapinfo #2 incorrect\n", s);
        print_mmapinfo(&inf2);
        exit(1);
      }

      ((char*) addr1)[PGSIZE + 42] = 'O';
      getmmapinfo(&inf2);
      if (inf2.total_mmaps != 1
          || inf2.addr[0] != addr1
          || inf2.length[0] != PGSIZE * 3
          || inf2.n_loaded_pages[0] != 2) {
        printf("%s: getmmapinfo #3 incorrect\n", s);
        print_mmapinfo(&inf2);
        exit(1);
      }

      ((char*) addr1)[PGSIZE*2 + 42] = 'O';
      getmmapinfo(&inf2);
      if (inf2.total_mmaps != 1
          || inf2.addr[0] != addr1
          || inf2.length[0] != PGSIZE * 3
          || inf2.n_loaded_pages[0] != 3) {
        printf("%s: getmmapinfo #4 incorrect\n", s);
        print_mmapinfo(&inf2);
        exit(1);
      }
    }

    if (unmap) {
      if (munmap(addr1) != 0) {
        printf("%s: munmap in child failed\n", s);
        exit(1);
      }
      getmmapinfo(&inf2);
      if (inf2.total_mmaps != 0) {
        printf("%s: getmmapinfo #5 incorrect\n", s);
        print_mmapinfo(&inf2);
        exit(1);
      }
    }

    exit(0);
  }

  // Child may have unmapped the mapping, but the parent should still have
  // `mmapinfo` be unchanged, except for if we've accessed the mapping in the
  // child:
  if (realized) {
    inf1.n_loaded_pages[0] = 3;
  }

  getmmapinfo(&inf2);
  if (mmapinfocmp(&inf1, &inf2) != 0) {
    printf("%s: mmapinfo changed in parent by child\n", s);
    print_mmapinfo(&inf1);
    print_mmapinfo(&inf2);
    exit(1);
  }

  addr2 = mmap(0, PGSIZE * 10, MAP_SHARED | MAP_ANONYMOUS);
  if (!addr2) {
    printf("%s: mmap #2 failed\n", s);
    exit(1);
  }

  getmmapinfo(&inf2);
  pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  } else if (pid != 0) {
    // Parent!
    wait(&wstatus);
    if (wstatus != 0) {
      printf("%s: child exited with non-zero status = %d\n", s, wstatus);
      exit(1);
    }
  } else {
    // Child, immediately fork again:
    pid = fork();
    if (pid < 0) {
      printf("%s: child fork failed\n", s);
      exit(1);
    } else if (pid != 0) {
      // Parent!
      wait(&wstatus);
      if (wstatus != 0) {
        printf("%s: grandchild exited with non-zero status = %d\n", s, wstatus);
        exit(1);
      }

      exit(0);
    }


    // Grandchild:
    getmmapinfo(&inf1);
    if (mmapinfocmp(&inf1, &inf2) != 0) {
      printf("%s: mmapinfo differs for grandchild\n", s);
      print_mmapinfo(&inf1);
      print_mmapinfo(&inf2);
      exit(1);
    }

    // Unmap the first mapping, write to the second, and add a third:
    if (unmap) {
      if (munmap(addr1) != 0) {
        printf("%s: munmap failed in grandchild\n", s);
        exit(1);
      }
    }

    if (realized) {
      ((char*) addr2)[PGSIZE*2 + 42] = 'x';
      ((char*) addr2)[PGSIZE*4 + 42] = 'v';
      ((char*) addr2)[PGSIZE*6 + 42] = '6';
    }

    if ((addr3 = mmap(0, PGSIZE, MAP_SHARED | MAP_ANONYMOUS)) == 0) {
      printf("%s: mmap failed in grandchild\n", s);
      exit(1);
    }

    getmmapinfo(&inf1);
    if (inf1.total_mmaps != ((unmap) ? 2 : 3)) {
      printf("%s: getmmapinfo wrong number of maps in grandchild\n", s);
      print_mmapinfo(&inf1);
      exit(1);
    }
    for (uint64 i = 0; i < 2; i++) {
      if (inf1.addr[i] == addr2) {
        // Make sure we have 3 pages mapped:
        if (inf1.length[i] != PGSIZE * 10 || inf1.n_loaded_pages[i] != ((realized) ? 3 : 0)) {
          printf("%s: getmmapinfo wrong information for addr2 mapping\n", s);
          print_mmapinfo(&inf1);
          exit(1);
        }
      } else if (inf1.addr[i] == addr3) {
        // Make sure we have no pages mapped:
        if (inf1.length[i] != PGSIZE || inf1.n_loaded_pages[i] != 0) {
          printf("%s: getmmapinfo wrong information for addr3 mapping\n", s);
          print_mmapinfo(&inf1);
          exit(1);
        }
      } else if (!unmap && inf1.addr[i] == addr1) {
        // If we're not unmapping, tolerate addr1
      } else {
        printf("%s: unknown address (%p) returned by getmmapinfo (in grandchild))\n", s, inf1.addr[i]);
        print_mmapinfo(&inf1);
        exit(1);
      }
    }

    exit(0);
  }

  getmmapinfo(&inf1);
  if (inf1.total_mmaps != 2) {
    printf("%s: getmmapinfo wrong number of maps in parent\n", s);
    print_mmapinfo(&inf1);
    exit(1);
  }
  for (uint64 i = 0; i < 2; i++) {
    if (inf1.addr[i] == addr1) {
      // Make sure we have 3 pages mapped:
      if (inf1.length[i] != PGSIZE * 3 || inf1.n_loaded_pages[i] != ((realized) ? 3 : 0)) {
        printf("%s: getmmapinfo wrong information for addr1 mapping\n", s);
        print_mmapinfo(&inf1);
        exit(1);
      }
    } else if (inf1.addr[i] == addr2) {
      // Make sure we have 3 pages mapped:
      if (inf1.length[i] != PGSIZE * 10 || inf1.n_loaded_pages[i] != ((realized) ? 3 : 0)) {
        printf("%s: getmmapinfo wrong information for addr2 mapping\n", s);
        print_mmapinfo(&inf1);
        exit(1);
      }
    } else {
      printf("%s: unknown address (%p) returned by getmmapinfo (in child)\n", s, inf1.addr[i]);
      print_mmapinfo(&inf1);
      exit(1);
    }
  }

  // Unmap all mappings in the parent:
  if (unmap) {
    if (munmap(addr1) != 0) {
      printf("%s: failed to unmap addr1 in parent\n", s);
      exit(1);
    }
    if (munmap(addr2) != 0) {
      printf("%s: failed to unmap addr2 in parent\n", s);
      exit(1);
    }
  }
}

void
mmap_fork_inheritance_nounmap(char *s)
{
  return mmap_fork_inheritance_base(s, 0, 0);
}

void
mmap_fork_inheritance_unmap(char *s)
{
  return mmap_fork_inheritance_base(s, 0, 1);
}

void
mmap_fork_inheritance_realized(char *s)
{
  return mmap_fork_inheritance_base(s, 1, 1);
}

// Make sure that mmap resources are cleaned up after a child exits:
void
mmap_exit_leak_base(char *s, int realized)
{
  int pid, wstatus;
  void *addr;

  for (uint64 i = 0; i < 64; i++) {
    addr = mmap(0, PGSIZE, MAP_ANONYMOUS | MAP_SHARED);
    if (!addr) {
      printf("%s: mmap failed\n", s);
      exit(1);
    }

    if (realized) {
      ((char*) addr)[42] = '!';
    }

    pid = fork();
    if (pid < 0) {
      printf("%s: fork failed\n", s);
      exit(1);
    } else if (pid == 0) {
      // Child! Just exit. This should decrement the refcnt:
      exit(0);
    }

    wait(&wstatus);
    if (wstatus != 0) {
      printf("%s: child returned non-zero exit status %d\n", s, wstatus);
      exit(1);
    }

    if (munmap(addr) != 0) {
      printf("%s: munmap failed\n", s);
      exit(1);
    }
  }

  // We've allocated a bunch of mappings, which were each referenced by a fork
  // that no longer exists, and a parent that unmapped them. We rely on the
  // usertests memory leak detector to find any leftover state in the kernel.
}

void
mmap_exit_leak_unrealized(char *s)
{
  return mmap_exit_leak_base(s, 0);
}

void
mmap_exit_leak_realized(char *s)
{
  return mmap_exit_leak_base(s, 1);
}

void mmap_exec_clear_helper() {
  struct mmapinfo info;
  getmmapinfo(&info);
  if (info.total_mmaps != 0) {
    printf("mmap_exec_clear_helper: mappings not cleared after exec\n");
    exit(1);
  }
  exit(0);
}

// Check that an `exec` system calls removes mmaps from the process.
void mmap_exec_clear(char *s) {
  int wstatus;
  void *addr;

  addr = mmap(0, PGSIZE, MAP_ANONYMOUS | MAP_SHARED);
  if (addr == 0) {
    printf("%s: mmap failed\n", s);
    exit(1);
  }

  int pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  } else if (pid == 0) {
    // Child! We want to `exec` another process, which should also clear our
    // mappings---mmaps don't persist across exec calls.
    //
    // The new process here exits with a code of 1 if it has any mappings.
    char *args[] = { progname, "mmap_exec_clear_helper", 0 };
    exec(progname, args);
    printf("%s: exec failed\n", s);
    exit(1);
  }

  wait(&wstatus);
  if (wstatus != 0) {
    printf("%s: child returned non-zero exit status %d\n", s, wstatus);
    exit(1);
  }
}

// Make sure that mmap resources are cleaned up after a child runs exec:
void
mmap_exec_leak_base(char *s, int realized)
{
  int pid, wstatus;
  void *addr;

  for (uint64 i = 0; i < 64; i++) {
    addr = mmap(0, PGSIZE, MAP_ANONYMOUS | MAP_SHARED);
    if (!addr) {
      printf("%s: mmap failed\n", s);
      exit(1);
    }

    if (realized) {
      ((char*) addr)[42] = '!';
    }

    pid = fork();
    if (pid < 0) {
      printf("%s: fork failed\n", s);
      exit(1);
    } else if (pid == 0) {
      // Child! We want to `exec` another process, which should also clear our
      // mappings---mmaps don't persist across exec calls.
      char *args[] = { "echo", 0 };
      exec("echo", args);
      printf("%s: exec failed\n", s);
      exit(1);
    }

    wait(&wstatus);
    if (wstatus != 0) {
      printf("%s: child returned non-zero exit status %d\n", s, wstatus);
      exit(1);
    }

    if (munmap(addr) != 0) {
      printf("%s: munmap failed\n", s);
      exit(1);
    }
  }

  // We've allocated a bunch of mappings, which were each referenced by a fork
  // that has `exec'd`, and a parent that unmapped them. We rely on the
  // usertests memory leak detector to find any leftover state in the kernel.
}

void
mmap_exec_leak_unrealized(char *s)
{
  return mmap_exec_leak_base(s, 0);
}

void
mmap_exec_leak_realized(char *s)
{
  return mmap_exec_leak_base(s, 1);
}

// Check that mmap mappings are zeroed:
void
mmap_zero_fill(char *s)
{
  void *addr;

  addr = mmap(0, PGSIZE * 16, MAP_ANONYMOUS | MAP_SHARED);
  if (!addr) {
    printf("%s: mmap failed\n", s);
    exit(1);
  }

  for (uint64 i = 0; i < PGSIZE * 16; i++) {
    if (((char*) addr)[i] != 0) {
      printf("%s: mmap not zero-initialized!\n", s);
      exit(1);
    }
  }

  if (munmap(addr) != 0) {
    printf("%s: munmap failed\n", s);
    exit(1);
  }
}

// Check that mmap writes can be read. This may not work if pages aren't mapped
// with the right permissions, and the fault handler maps different memory for
// read and write.
void
mmap_read_written(char *s)
{
  void *addr;

  addr = mmap(0, PGSIZE, MAP_ANONYMOUS | MAP_SHARED);
  if (!addr) {
    printf("%s: mmap failed\n", s);
    exit(1);
  }

  ((char*) addr)[42] = '!';
  if (((char*) addr)[42] != '!') {
    printf("%s: read value does not correspond to prior write\n", s);
  }

  if (munmap(addr) != 0) {
    printf("%s: munmap failed\n", s);
    exit(1);
  }
}

void
mmap_post_fork_write_by_child(char *s)
{
  int pid, wstatus;
  void *addr;

  addr = mmap(0, PGSIZE * 16, MAP_ANONYMOUS | MAP_SHARED);
  if (!addr) {
    printf("%s: mmap failed\n", s);
    exit(1);
  }

  pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  } else if (pid == 0) {
    // Child! Write to the mapping, causing a fault:
    ((char*) addr)[42] = '!';
    exit(0);
  }

  // In the parent, wait for the child to successfully exit:
  wait(&wstatus);
  if (wstatus != 0) {
    printf("%s: child exited with non-zero status = %d\n", s, wstatus);
    exit(1);
  }

  // Make sure the child's write is visible in the parent:
  if (((char*) addr)[42] != '!') {
    printf("%s: child's write is not visible in parent\n", s);
    exit(1);
  }

  if (munmap(addr) != 0) {
    printf("%s: munmap failed\n", s);
    exit(1);
  }
}

// Check that a write by a grandchild is visible to the parent
void
mmap_post_fork_write_by_grandchild(char *s)
{
  int pid, wstatus;
  void *addr;

  addr = mmap(0, PGSIZE * 16, MAP_ANONYMOUS | MAP_SHARED);
  if (!addr) {
    printf("%s: mmap failed\n", s);
    exit(1);
  }

  pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  } else if (pid == 0) {
    // Child!
    pid = fork();
    if (pid < 0) {
      printf("%s: child fork failed\n", s);
      exit(1);
    } else if (pid == 0) {
      // Grandchild! Write to the mapping, causing a fault:
      ((char*) addr)[42] = '!';
      exit(0);
    }

    // Wait for grandchild:
    wait(&wstatus);
    if (wstatus != 0) {
      printf("%s: grandchild exited with non-zero status = %d\n", s, wstatus);
      exit(1);
    }

    // Exit the child:
    exit(0);
  }

  // In the parent, wait for the child to successfully exit:
  wait(&wstatus);
  if (wstatus != 0) {
    printf("%s: child exited with non-zero status = %d\n", s, wstatus);
    exit(1);
  }

  // Make sure the child's write is visible in the parent:
  if (((char*) addr)[42] != '!') {
    printf("%s: child's write is not visible in parent\n", s);
    exit(1);
  }

  if (munmap(addr) != 0) {
    printf("%s: munmap failed\n", s);
    exit(1);
  }
}

// Check that a write by a one child (with pagefault post-fork) is
// visible to a sibling:
void
mmap_post_fork_write_by_sibling(char *s)
{
  char c;
  int pid1, pid2, wpid, wstatus, p[2];
  void *addr;

  if (pipe(p) < 0) {
    printf("%s: allocating pipe failed\n", s);
    exit(1);
  }

  addr = mmap(0, PGSIZE * 16, MAP_ANONYMOUS | MAP_SHARED);
  if (!addr) {
    printf("%s: mmap failed\n", s);
    exit(1);
  }

  pid1 = fork();
  if (pid1 < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  } else if (pid1 == 0) {
    // First Child! Write to the mapping, causing a pagefault:
    ((char*) addr)[42] = '!';
    exit(0);
  }

  pid2 = fork();
  if (pid2 < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  } else if (pid2 == 0) {
    // Second Child! Wait on the pipe, and then check that the read is visible:
    if (read(p[0], &c, 1) < 0) {
      printf("%s: child pipe read failed\n", s);
      exit(1);
    }

    if (((char*) addr)[42] != '!') {
      printf("%s: child did not see sibling's write\n", s);
      exit(1);
    }

    exit(0);
  }

  // In the parent, wait for the child to successfully exit:
  wpid = wait(&wstatus);
  if (wpid != pid1) {
    printf("%s: wait did not return first child's pid\n", s);
    exit(1);
  } else if (wstatus != 0) {
    printf("%s: first child exited with non-zero status = %d\n", s, wstatus);
    exit(1);
  }

  // Now, signal to the second pipe:
  if (write(p[1], " ", 1) < 0) {
    printf("%s: parent pipe write failed\n", s);
    exit(1);
  }

  // Finally, wait for the second child to successfully exit:
  wait(&wstatus);
  if (wstatus != 0) {
    printf("%s: second child exited with non-zero status = %d\n", s, wstatus);
    exit(1);
  }

  if (munmap(addr) != 0) {
    printf("%s: munmap failed\n", s);
    exit(1);
  }
}


// Ensure that a child can see the parent's write, when fork()ing before the
// mmap pagefault:
void
mmap_post_fork_write_by_parent(char *s)
{
  char c;
  int pid, wstatus, p[2];
  void *addr;

  if (pipe(p) < 0) {
    printf("%s: allocating pipe failed\n", s);
    exit(1);
  }

  addr = mmap(0, PGSIZE * 16, MAP_ANONYMOUS | MAP_SHARED);
  if (!addr) {
    printf("%s: mmap failed\n", s);
    exit(1);
  }

  pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  } else if (pid == 0) {
    // Child! Read the parent's write, but only after synchronizing by waiting
    // for the parent to write to the pipe:
    if (read(p[0], &c, 1) < 0) {
      printf("%s: child pipe read failed\n", s);
      exit(1);
    }

    // We've heard from the parent, now check if we can see its write:
    if (((char*) addr)[42] != '!') {
      printf("%s: parent's write is not visible in child\n", s);
      exit(1);
    }

    exit(0);
  }

  // In the parent, perform the write:
  ((char*) addr)[42] = '!';

  // Now, signal the child that we've written:
  if (write(p[1], " ", 1) < 0) {
    printf("%s: parent pipe write failed\n", s);
    exit(1);
  }

  // Finally, wait for the child to exit successfully:
  wait(&wstatus);
  if (wstatus != 0) {
    printf("%s: child exited with non-zero status = %d\n", s, wstatus);
    exit(1);
  }

  if (munmap(addr) != 0) {
    printf("%s: munmap failed\n", s);
    exit(1);
  }
}

// Ensure that a child can see the parent's write, when the mmap pagefault
// occurs before fork()ing:
void
mmap_pre_fork_write(char *s)
{
  char c;
  int pid, wstatus, p[2];
  void *addr;

  if (pipe(p) < 0) {
    printf("%s: allocating pipe failed\n", s);
    exit(1);
  }

  addr = mmap(0, PGSIZE * 16, MAP_ANONYMOUS | MAP_SHARED);
  if (!addr) {
    printf("%s: mmap failed\n", s);
    exit(1);
  }

  // Force an mmap pagefault before the fork:
  ((char*) addr)[42] = '?';

  pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  } else if (pid == 0) {
    // Child! Read the parent's write, but only after synchronizing by waiting
    // for the parent to write to the pipe:
    if (read(p[0], &c, 1) < 0) {
      printf("%s: child pipe read failed\n", s);
      exit(1);
    }

    // We've heard from the parent, now check if we can see its write:
    if (((char*) addr)[42] != '!') {
      printf("%s: parent's write is not visible in child\n", s);
      exit(1);
    }

    exit(0);
  }

  // In the parent, perform the write:
  ((char*) addr)[42] = '!';

  // Now, signal the child that we've written:
  if (write(p[1], " ", 1) < 0) {
    printf("%s: parent pipe write failed\n", s);
    exit(1);
  }

  // Finally, wait for the child to exit successfully:
  wait(&wstatus);
  if (wstatus != 0) {
    printf("%s: child exited with non-zero status = %d\n", s, wstatus);
    exit(1);
  }

  if (munmap(addr) != 0) {
    printf("%s: munmap failed\n", s);
    exit(1);
  }
}

// Make sure the system can allocate more memory in mmaps than we have available
// physical memory. For MAX_MMAPS = 64 and MAX_MMAP_LEN = 2MB, this is 128
// MB. While we do have 128 MB physical memory, some of this is already used for
// the kernel and other userspace processes:
void
mmap_overcommit_base(char *s, int realized)
{
  struct mmapinfo inf;
  void *addrs[MAX_MMAPS];

  for (int i = 0; i < MAX_MMAPS; i++) {
    // mmaps should support at least 2MB mappings:
    addrs[i] = mmap(0, 2 * 1024 * 1024, MAP_ANONYMOUS | MAP_SHARED);
    if (!addrs[i]) {
      printf("%s: mmap failed!\n", s);
      exit(1);
    }

    // Make sure we can write at least the first and last page of this mapping:
    if (realized) {
      ((char*) addrs[i])[42] = '!';
      ((char*) addrs[i])[2 * 1024 * 1024 - 42] = '!';
    }
  }

  getmmapinfo(&inf);
  if (inf.total_mmaps != MAX_MMAPS) {
    printf("%s: getmmapinfo: total mmaps incorrect!\n", s);
  }

  for (int i = 0; i < inf.total_mmaps; i++) {
    if (inf.length[i] != 2 * 1024 * 1024 || inf.n_loaded_pages[i] != ((realized) ? 2 : 0)) {
      printf("%s: getmmapinfo incorrect for mapping %d\n", s, i);
      exit(1);
    }

    if (munmap(addrs[i]) != 0) {
      printf("%s: munmap failed for mapping %d\n", s, i);
    }
  }
}

void
mmap_overcommit_unrealized(char *s)
{
  return mmap_overcommit_base(s, 0);
}

void
mmap_overcommit_realized(char *s)
{
  return mmap_overcommit_base(s, 1);
}

void
mmap_oom_fault(char *s)
{
  int pid, wstatus;
  void *addrs[MAX_MMAPS];

  for (int i = 0; i < MAX_MMAPS; i++) {
    // mmaps should support at least 2MB mappings:
    addrs[i] = mmap(0, 2 * 1024 * 1024, MAP_ANONYMOUS | MAP_SHARED);
    if (!addrs[i]) {
      printf("%s: mmap failed!\n", s);
      exit(1);
    }
  }

  pid = fork();
  if (pid < 0) {
    printf("%s: fork failed!\n", s);
    exit(1);
  } else if (pid == 0) {
    // In the child, write to all the pages in the mappings until we're OOM:
    for (int i = 0; i < MAX_MMAPS; i++) {

      for (void* a = addrs[i]; a < (addrs[i] + 2 * 1024 * 1024); a += PGSIZE) {
        *((char*) a) = '!';
      }
    }

    exit(0);
  }

  // In the parent, make sure the child is killed by the kernel:
  wait(&wstatus);
  if (wstatus != -1) {
    printf("%s: child did not run OOM! (wstatus = %d)\n", s, wstatus);
    exit(1);
  }

  for (int i = 0; i < MAX_MMAPS; i++) {
    if (munmap(addrs[i]) != 0) {
      printf("%s: munmap failed for mapping %d\n", s, i);
    }
  }
}

// Check that accesses before and after the mapping fail:
void
mmap_out_of_bounds_fault(char *s)
{
  int pid, wstatus;
  void *addr;

  addr = mmap((void*) 0x70000000, PGSIZE * 3, MAP_ANONYMOUS | MAP_SHARED);
  if (addr == 0) {
    printf("%s: mmap failed\n", s);
    exit(1);
  }

  pid = fork();
  if (pid < 0) {
    printf("%s: fork failed!\n", s);
    exit(1);
  } else if (pid == 0) {
    // Child! Try to access just before the mapping:
    *((char*) addr - 1) = '!';
    exit(0);
  }

  wait(&wstatus);
  if (wstatus == 0) {
    printf("%s: child didn't fault accessing data before mapping", s);
    exit(1);
  }

  pid = fork();
  if (pid < 0) {
    printf("%s: fork failed!\n", s);
    exit(1);
  } else if (pid == 0) {
    // Child! Try to access just before the mapping:
    *((char*) addr + PGSIZE * 3) = '!';
    exit(0);
  }

  wait(&wstatus);
  if (wstatus == 0) {
    printf("%s: child didn't fault accessing data after mapping", s);
    exit(1);
  }

  if (munmap(addr) != 0) {
    printf("%s: munmap failed!\n", s);
    exit(1);
  }
}

// Check that unmapping in an intermediate process does not destroy the mapping:
void
mmap_middle_unmap(char *s)
{
  char c;
  int pid, wstatus, p[2];
  void *addr;

  addr = mmap(0, PGSIZE * 16, MAP_ANONYMOUS | MAP_SHARED);
  if (!addr) {
    printf("%s: mmap failed\n", s);
    exit(1);
  }

  pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  } else if (pid == 0) {
    // Child!
    if (pipe(p) < 0) {
      printf("%s: allocating pipe failed\n", s);
      exit(1);
    }

    pid = fork();
    if (pid < 0) {
      printf("%s: child fork failed\n", s);
      exit(1);
    } else if (pid == 0) {
      // Grandchild!

      // Wait for first child to unmap:
      if (read(p[0], &c, 1) < 0) {
        printf("%s: grandchild pipe read failed\n", s);
        exit(1);
      }

      // Write to the mapping, causing a fault:
      ((char*) addr)[42] = '!';
      exit(0);
    }

    // Unmap in the first child:
    if (munmap(addr) != 0) {
      printf("%s: munmap failed\n", s);
      exit(1);
    }

    // Now, signal to the grandpipe pipe:
    if (write(p[1], " ", 1) < 0) {
      printf("%s: parent pipe write failed\n", s);
      exit(1);
    }

    // Wait for grandchild:
    wait(&wstatus);
    if (wstatus != 0) {
      printf("%s: grandchild exited with non-zero status = %d\n", s, wstatus);
      exit(1);
    }

    // Exit the child:
    exit(0);
  }

  // In the parent, wait for the child to successfully exit:
  wait(&wstatus);
  if (wstatus != 0) {
    printf("%s: child exited with non-zero status = %d\n", s, wstatus);
    exit(1);
  }

  // Make sure the grandchild's write is visible in the parent:
  if (((char*) addr)[42] != '!') {
    printf("%s: child's write is not visible in parent\n", s);
    exit(1);
  }

  if (munmap(addr) != 0) {
    printf("%s: munmap failed\n", s);
    exit(1);
  }
}

// Check that an intermediate process dying does not destroy the mapping:
void
mmap_orphan(char *s)
{
  char c;
  int pid, wstatus, wpid, p1[2], p2[2];
  void *addr;

  if (pipe(p1) < 0 || pipe(p2) < 0) {
    printf("%s: allocating pipe failed\n", s);
    exit(1);
  }

  addr = mmap(0, PGSIZE * 16, MAP_ANONYMOUS | MAP_SHARED);
  if (!addr) {
    printf("%s: mmap failed\n", s);
    exit(1);
  }

  pid = fork();
  if (pid < 0) {
    printf("%s: fork failed\n", s);
    exit(1);
  } else if (pid == 0) {
    // Child!
    pid = fork();
    if (pid < 0) {
      printf("%s: child fork failed\n", s);
      exit(1);
    } else if (pid == 0) {
      // Grandchild!

      // Wait for first child to exit:
      if (read(p1[0], &c, 1) < 0) {
        printf("%s: grandchild pipe read failed\n", s);
        exit(1);
      }

      // Write to the mapping, causing a fault:
      ((char*) addr)[42] = '!';

      // Signal that we've written:
      if (write(p2[1], " ", 1) < 0) {
        printf("%s: grandchild pipe write failed\n", s);
        exit(1);
      }

      // Exit the grandchild:
      exit(0);
    }

    // Exit the first child:
    exit(0);
  }

  // In the parent, wait for the first child to successfully exit:
  wpid = wait(&wstatus);
  if (wpid != pid) {
    printf("%s: wait returned wrong pid\n", s);
    exit(1);
  } else if (wstatus != 0) {
    printf("%s: child exited with non-zero status = %d\n", s, wstatus);
    exit(1);
  }

  // Now, signal to the grandchild that its parent has died:
  if (write(p1[1], " ", 1) < 0) {
    printf("%s: parent pipe write failed\n", s);
    exit(1);
  }

  // Wait for the grandchild to complete its write:
  if (read(p2[0], &c, 1) < 0) {
    printf("%s: parent pipe read failed\n", s);
    exit(1);
  }

  // Make sure the grandchild's write is visible in the parent:
  if (((char*) addr)[42] != '!') {
    printf("%s: child's write is not visible in parent\n", s);
    exit(1);
  }

  if (munmap(addr) != 0) {
    printf("%s: munmap failed\n", s);
    exit(1);
  }
}

struct test {
  void (*f)(char *);
  char *s;
};

struct testgroup {
  char *name;
  struct test *tests;
  struct testgroup *groups[];
};

struct test group_task3_tests[] = {
  {mmap_illegal_flags, "mmap_illegal_flags"},
  {mmap_zero_length, "mmap_zero_length"},
  {mmap_unaligned, "mmap_unaligned"},
  {mmap_not_fixed_addr_selection, "mmap_not_fixed_addr_selection"},
  {mmap_length_roundup_nounmap, "mmap_length_roundup_nounmap"},
  {mmap_before_brk_fail, "mmap_before_brk_fail"},
  {mmap_high_mappings_collision, "mmap_high_mappings_collision"},
  {mmap_exceed_max_mmaps_nounmap, "mmap_exceed_max_mmaps_nounmap"},
  {mmap_brk_into_mapping_fail_unrealized_nounmap, "mmap_brk_into_mapping_fail_unrealized_nounmap"},
  {mmap_collisions_nounmap, "mmap_collisions_nounmap"},
  {mmap_respects_addr_hint, "mmap_respects_addr_hint"},
  {mmap_fork_inheritance_nounmap, "mmap_fork_inheritance_nounmap"},
  {0, 0},
};

struct testgroup group_task3 = {
  .name = "task3",
  .tests = group_task3_tests,
  .groups = {0},
};

struct test group_task4_tests[] = {
  {munmap_invalid_address, "munmap_invalid_address"},
  {munmap_reject_offset, "munmap_reject_offset"},
  {mmap_length_roundup_unmap, "mmap_length_roundup_unmap"},
  {mmap_exceed_max_mmaps_unmap, "mmap_exceed_max_mmaps_unmap"},
  {mmap_brk_into_mapping_fail_unrealized_unmap, "mmap_brk_into_mapping_fail_unrealized_unmap"},
  {mmap_collisions_unmap, "mmap_collisions_unmap"},
  {mmap_exit_leak_unrealized, "mmap_exit_leak_unrealized"},
  {mmap_exec_clear, "mmap_exec_clear"},
  {mmap_exec_leak_unrealized, "mmap_exec_leak_unrealized"},
  {mmap_overcommit_unrealized, "mmap_overcommit_unrealized"},
  {mmap_fork_inheritance_unmap, "mmap_fork_inheritance_unmap"},
  {0, 0},
};

struct testgroup group_task4 = {
  .name = "task4",
  .tests = group_task4_tests,
  .groups = {
    &group_task3,
    0
  },
};

struct test group_task5_tests[] = {
  {mmap_larger_than_2mb, "mmap_larger_than_2mb"},
  {mmap_length_roundup_realized, "mmap_length_roundup_realized"},
  {mmap_brk_into_mapping_fail_realized, "mmap_brk_into_mapping_fail_realized"},
  {mmap_collisions_realized, "mmap_collisions_realized"},
  {mmap_exit_leak_realized, "mmap_exit_leak_realized"},
  {mmap_exec_leak_realized, "mmap_exec_leak_realized"},
  {mmap_zero_fill, "mmap_zero_fill"},
  {mmap_read_written, "mmap_read_written"},
  {mmap_post_fork_write_by_child, "mmap_post_fork_write_by_child"},
  {mmap_post_fork_write_by_grandchild, "mmap_post_fork_write_by_grandchild"},
  {mmap_post_fork_write_by_parent, "mmap_post_fork_write_by_parent"},
  {mmap_pre_fork_write, "mmap_pre_fork_write"},
  {mmap_overcommit_realized, "mmap_overcommit_realized"},
  {mmap_oom_fault, "mmap_oom_fault"},
  {mmap_out_of_bounds_fault, "mmap_out_of_bounds_fault"},
  {mmap_post_fork_write_by_sibling, "mmap_post_fork_write_by_sibling"},
  {mmap_middle_unmap, "mmap_middle_unmap"},
  {mmap_orphan, "mmap_orphan"},
  {mmap_fork_inheritance_realized, "mmap_fork_inheritance_realized"},
  {0, 0},
};

struct testgroup group_task5 = {
  .name = "task5",
  .tests = group_task5_tests,
  .groups = {
    &group_task4,
    0
  },
};

struct testgroup group_alltests = {
  .name = "alltests",
  .tests = 0,
  .groups = {
    &group_task5,
    0
  },
};

//
// drive tests
//

// run each test in its own process. run returns 1 if child's exit()
// indicates success.
int
run(void f(char *), char *s) {
  int pid;
  int xstatus;

  printf("test %s: ", s);
  if((pid = fork()) < 0) {
    printf("runtest: fork error\n");
    exit(1);
  }
  if(pid == 0) {
    f(s);
    exit(0);
  } else {
    wait(&xstatus);
    if(xstatus != 0)
      printf("FAILED\n");
    else
      printf("OK\n");
    return xstatus == 0;
  }
}


// use sbrk() to count how many free physical memory pages there are.
int
countfree()
{
  int n = 0;
  uint64 sz0 = (uint64)sbrk(0);
  while(1){
    char *a = sbrk(PGSIZE);
    if(a == SBRK_ERROR){
      break;
    }
    n += 1;
  }
  sbrk(-((uint64)sbrk(0) - sz0));
  return n;
}

// Recursively find a test group by name
struct testgroup* findgroup(struct testgroup *g, char *name) {
  if (!g) return 0;
  if (strcmp(g->name, name) == 0) return g;
  for (int i = 0; g->groups[i] != 0; i++) {
    struct testgroup *found = findgroup(g->groups[i], name);
    if (found) return found;
  }
  return 0;
}

int runtests(struct test *tests, char *justone, int continuous) {
  if (!tests) return 0; // Prevent null pointer dereference on group_alltests
  int ntests = 0;
  for (struct test *t = tests; t->s != 0; t++) {
    if((justone == 0) || strcmp(t->s, justone) == 0) {
      ntests++;
      if(!run(t->f, t->s)){
        if(continuous != 2){
          printf("SOME TESTS FAILED\n");
          return -1;
        }
      }
    }
  }
  return ntests;
}

// Recursively execute all tests in a group and its subgroups
int rungroup(struct testgroup *g, char *justone, int continuous) {
  if (!g) return 0;
  int ntests = 0;
  int n = 0;

  for (int i = 0; g->groups[i] != 0; i++) {
    n = rungroup(g->groups[i], justone, continuous);
    if (n < 0 && continuous != 2) return -1;
    ntests += n;
  }

  if (g->tests) {
    if (!justone)
      printf("Running tests from group %s\n", g->name);
    n = runtests(g->tests, justone, continuous);
    if (n < 0 && continuous != 2) return -1;
    ntests += n;
  }

  return ntests;
}

int drivetests(struct testgroup *target, int continuous, char *justone) {
  do {
    printf("%s starting\n", progname);
    int free0 = countfree();
    int free1 = 0;

    int ntests = rungroup(target, justone, continuous);
    if (ntests < 0) {
      if (continuous != 2) {
        return 1;
      }
    }

    if((free1 = countfree()) < free0) {
      printf("FAILED -- lost some free pages %d (out of %d)\n", free1, free0);
      if(continuous != 2) {
        return 1;
      }
    }
    if (justone != 0 && ntests == 0) {
      printf("NO TESTS EXECUTED\n");
      return 1;
    }
  } while(continuous);
  return 0;
}

int main(int argc, char *argv[]) {
  progname = argv[0];

  int continuous = 0;
  char *justone = 0;
  char *groupname = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-c") == 0) continuous = 1;
    else if (strcmp(argv[i], "-C") == 0) continuous = 2;
    else if (strcmp(argv[i], "-g") == 0) {
      if (++i < argc) groupname = argv[i];
      else goto usage;
    }
    else if (argv[i][0] != '-') justone = argv[i];
    else goto usage;
  }

  // Special case the "mmap_exec_clear_helper", which is exec'd from
  // the "mmap_exec_clear" test case:
  if (justone && strcmp(justone, "mmap_exec_clear_helper") == 0) {
    mmap_exec_clear_helper();
    return 0;
  }

  struct testgroup *target = &group_alltests;
  if (groupname) {
    target = findgroup(&group_alltests, groupname);
    if (!target) {
      printf("Group %s not found\n", groupname);
      exit(1);
    }
  }

  if (drivetests(target, continuous, justone)) {
    exit(1);
  }
  printf("ALL TESTS PASSED\n");
  exit(0);

usage:
  printf("Usage: %s [-c] [-C] [-g group] [testname]\n", progname);
  exit(1);
}
