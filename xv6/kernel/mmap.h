// Maximum number of `mmap`s per process:
#define MAX_MMAPS 64

// Flags for the `mmap` system call:
#define MAP_SHARED 0x0002
#define MAP_ANONYMOUS 0x0004
#define MAP_FIXED 0x0008

// Struct filled by the `getmmapinfo` system call:
struct mmapinfo {
    uint64 total_mmaps;               // Total number of mmap regions
    void* addr[MAX_MMAPS];            // Starting address of each mapping
    uint64 length[MAX_MMAPS];         // Size of each mapping
    uint64 n_loaded_pages[MAX_MMAPS]; // Number of pages physically loaded into
                                      // memory across all mappings
};
