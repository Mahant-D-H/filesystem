Building a database-optimized filesystem requires abandoning traditional general-purpose POSIX abstractions (like kernel page-caching, indirect block structures, and standard POSIX locks) in favor of direct block-device control, zero-copy asynchronous I/O, and specialized layout engines.

---

## Phase 1: Storage Device Subsystem & Asynchronous I/O

Standard filesystems rely on the OS page cache, causing double-buffering when a database manages its own cache pool. Your filesystem must interact directly with storage blocks without kernel cache intervention.

### Core Concepts

* **Block Devices & Memory Alignment:** Modern NVMe drives read and write in physical sectors ($4096$ bytes). Buffers sent to the controller must be aligned to memory address boundaries divisible by the sector size ($4\text{KB}$ alignment via `posix_memalign` or `aligned_alloc`).
* **Bypassing the Page Cache:** Open the storage block device or underlying raw image file using `O_DIRECT | O_SYNC` to bypass kernel caching and ensure writes reach physical media.
* **Kernel-Bypass / Async I/O Engines:** Synchronous `pread`/`pwrite` calls block execution threads. Modern database storage layers use **`io_uring`** (Linux) to submit completion queues directly to the kernel without context switches.

### Implementation Milestones

1. Build a `BlockDevice` class that opens a target device/file descriptor using `O_DIRECT`.
2. Implement an asynchronous I/O driver wrapper using `io_uring` with ring submission/completion queues (`io_uring_queue_init`, `io_uring_submit`).
3. Define the core address unit: $\text{Byte Offset} = \text{LBA} \times \text{Block Size}$, where $\text{LBA}$ is the Logical Block Address.

---

## Phase 2: Metadata Architecture, Extents, & Space Allocation

General-purpose filesystems allocate single blocks randomly, causing disk fragmentation. Databases require contiguous memory regions for high-throughput scans.

### Core Concepts

* **Extents vs. Blocks:** Instead of mapping individual $4\text{KB}$ blocks to files, allocate contiguous spans of blocks called **Extents** ($\text{Extent} = \{\text{Start LBA}, \text{Block Count}\}$).
* **Superblock Layout:** Header stored at $\text{LBA}_0$ containing filesystem metadata: block size, total sectors, layout map offset, and magic identification bytes.
* **Space Tracking Allocator:**
* **Buddy Allocator:** Excellent for fast, fixed-size power-of-two allocation.
* **Free Space Extent Tree (B+ Tree / Segment Tree):** Ideal for tracking contiguous free regions to fit large contiguous files (SSTables, vector index dumps).



### Metadata Layout Structure

| On-Disk Section | Offset / Location | Purpose |
| --- | --- | --- |
| **Superblock** | $\text{LBA}_0$ ($0$ – $4096$ bytes) | FS Version, total blocks, pointers to allocation maps |
| **Allocation Map** | $\text{LBA}_1$ to $\text{LBA}_k$ | Extent free-lists or bitmap tracking free storage |
| **Inode Table** | $\text{LBA}_{k+1}$ to $\text{LBA}_m$ | Data structure representing files, file sizes, and extent arrays |
| **Data Region** | $\text{LBA}_{m+1}$ to $\text{End}$ | Contiguous physical block extents storing user/db data |

---

## Phase 3: Buffer Pool & Custom Page Management

Because `O_DIRECT` bypasses kernel memory, your filesystem must supply its own high-performance page frame buffer pool to cache metadata and hot data blocks in RAM.

### Core Concepts

* **Frame Buffers & Page Pinning:** Maintain a fixed pool of aligned $4\text{KB}$ memory frames in RAM. When a file system layer requests Page $N$, it locks ("pins") the buffer frame so an eviction thread does not write over active data.
* **Eviction Policies for DB Workloads:** Standard Least Recently Used (LRU) fails under database table scans (which flush the entire cache). Use **2Q** or **CLOCK-Pro** eviction to differentiate between sequential scans and frequently accessed index pages.
* **Dirty Page Flushing:** Maintain a dirty list with background async flush workers to commit modified pages back to the `io_uring` ring.

---

## Phase 4: Database Workload-Specific Layout Engines

Different database engines possess distinct I/O patterns. A high-performance storage engine provides multi-modal physical storage layouts based on access types:

```
                  ┌─────────────────────────────────────────┐
                  │      Custom Filesystem Abstraction      │
                  └────────────────────┬────────────────────┘
                                       │
        ┌──────────────────────────────┼──────────────────────────────┐
        ▼                              ▼                              ▼
┌───────────────┐              ┌───────────────┐              ┌───────────────┐
│ Fixed-Page Engine │          │ Append-Only   │              │ Columnar &    │
│  (Random I/O) │              │ Segment Engine│              │ Dense Vector  │
└───────┬───────┘              └───────┬───────┘              └───────┬───────┘
        │                              │                              │
        ▼                              ▼                              ▼
SQL (Postgres, MySQL)         LSM-Trees (RocksDB,             Vector (Qdrant, Milvus),
In-Place Page Updates         MongoDB SSTables)               Time-Series (InfluxDB)

```

### 1. Random Access / Fixed-Page Layout (SQL Workloads)

* **Target:** PostgreSQL, MySQL, CockroachDB.
* **Design:** Files organized as arrays of fixed pages ($8\text{KB}$ or $16\text{KB}$).
* **Mechanism:** Supports high-speed, direct random offsets:

$$\text{Page Offset} = \text{File Header Offset} + (\text{Page ID} \times \text{Page Size})$$


* Requires in-place updates and explicit dirty-page tracking.

### 2. Append-Only / Immutable Segment Engine (LSM & Search Workloads)

* **Target:** Cassandra, RocksDB/LSM SSTables, Elasticsearch inverted indexes.
* **Design:** Writes are strictly sequential and append-only. Pages are written once and never updated in place.
* **Mechanism:** Allocate contiguous large extents ($64\text{MB}$ or $128\text{MB}$) to eliminate write amplification and maximize write throughput.

### 3. Memory-Mapped / Dense Array Layout (Vector & Time-Series Workloads)

* **Target:** Qdrant, Milvus (HNSW graphs), InfluxDB (Columnar time chunks).
* **Design:** High-dimensional vector graphs require random node traversals across millions of dense float arrays. Time-series requires fast range scans over compressed timestamps.
* **Mechanism:** Pre-allocated, zero-fragmentation huge extents aligned to $2\text{MB}$ boundaries (matching CPU HugePages) to accelerate cache hits during graph traversals.

---

## Phase 5: Crash Consistency, WAL, and Transactions

Physical power loss mid-write causes corrupted inodes or orphan block allocations. You must guarantee structural integrity.

### Core Concepts

* **Write-Ahead Logging (WAL):** Before updating an inode or extent allocation map on disk, write the intended operation to an append-only ring buffer log segment on physical storage.
* **Double-Write Buffer:** For page updates (SQL engines), partial page writes (torn writes) can happen if power drops mid-4KB sector flush. Maintain a separate contiguous "double-write zone" where pages are written sequentially before updating their target location.
* **Checkpointing & Recovery:**
1. On boot, parse the Superblock and locate the last valid log sequence number (LSN).
2. Play forward uncommitted valid transactions from the log (Redo phase).
3. Reclaim allocated extents that lack corresponding valid inode pointers (Undo phase).

### Current Implementation

Format version 3 reserves mirrored superblocks at LBA 0 and 1, a 16-block WAL, and an 8-block double-write zone. The single-writer WAL records checksummed 4 KiB page images and commit records; a transaction may update up to seven distinct blocks. Inode-table and allocation-bitmap changes use this path, as do fixed-page and dense-array writes. Append-only payload data is flushed before its inode size is committed.

Mount recovery replays only complete committed WAL transactions, checkpoints their LSN, then reconstructs inode and data allocation maps from valid inode extent references to reclaim orphaned allocations. Recovery is idempotent. The fixed WAL capacity and single-writer model are intentional limits until the concurrency work in Phase 6.

---

## Phase 6: Thread-per-Core Architecture & Concurrency

To maximize NVMe drives capable of millions of IOPS, avoid central locks (like `std::mutex`) across I/O worker threads.

### Architecture Plan

* **Thread-per-Core Execution:** Pin $N$ worker threads to $N$ CPU cores using `sched_setaffinity`.
* **Shared-Nothing Queue Partitioning:** Each CPU core owns its own instance of `io_uring`, its own subset of free extents, and a lock-free queue.
* **Lock-Free Extent Allocation:** Use lock-free ring buffers (`boost::lockfree::queue` or custom atomic CAS queues) to pass block allocation requests across cores without locking the superblock allocator.