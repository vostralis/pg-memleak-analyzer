# pg-memleak-analyzer

`pg-memleak-analyzer` is a diagnostic tool and a PostgreSQL extension designed to analyze and localize logical memory leaks inside PostgreSQL backend sessions and background workers. 

The tool implements a **hybrid architecture** combining a dynamically loaded C extension with a lightweight patch to the PostgreSQL core.

---

## Key Features

- **MemoryContext Traversal:** Recursively traverses the `MemoryContext` hierarchy to measure used space.
- **Differential Analysis:** Compares pre-execution and post-execution memory states to calculate the exact bytes leaked or retained per context.
- **Background Worker Profiling:** Profiles running, isolated background workers asynchronously via custom process signals and static shared memory transport.
- **GUC Configuration:** Runtime configuration support.

---

## Prerequisites & Installation

This project was implemented as a part of a graduation thesis and utilizes a custom PostgreSQL core patch to function correctly. The patch is based on an unmerged PostgreSQL community architectural proposal from 2020 ([view community discussion archives](https://www.postgresql.org/message-id/CALT9ZEFRK6VFHDm9SJ112PFvio%3Defd20MNYkKJR2Tz%2BFFZe2yQ%40mail.gmail.com)) that introduces dynamic custom process signal registration. This proposal has been slightly changed to run on newer PostgreSQL database engines.

### Step 1: Apply the PostgreSQL Core Patch

Because vanilla PostgreSQL does not support the dynamic registration of custom `ProcSignal` interrupts from extensions, you must apply the provided patch to the PostgreSQL source code and compile the server from source.

1. Clone this repository:
```bash
git clone https://github.com/vostralis/pg-memleak-analyzer.git
```

2. Clone PostgreSQL source repository:
```bash
git clone https://github.com/postgres/postgres.git
cd postgres
```

3. Apply the patch:
```bash
git apply /path/to/pg-memleak-analyzer/patches/procsignal_custom.patch
```

3. Build and install PostgreSQL:
```bash
./configure --prefix=/path/to/install
make && make install
```

### Step 2: Build and Install the Extension

Before compiling the extension, you must ensure that the `pg_config` executable from your patched PostgreSQL installation is available in your system's `PATH`.

1. Export the path to your custom PostgreSQL binary directory:
```bash
export PATH=/path/to/install/bin:$PATH
```

2. Compile and install the extension:

```bash
cd /path/to/pg-memleak-analyzer
make && make install
```

### Step 3: Configure `postgresql.conf`

To load the extension at startup, add it to `shared_preload_libraries` in your `postgresql.conf`:

```bash
shared_preload_libraries = 'memleak_analyzer'
```

Restart your PostgreSQL server to apply changes.

---

## SQL installation

Connect to your target database and register the extension:

```sql
CREATE EXTENSION memleak_analyzer;
```

---

## Configuration reference (GUCs)

The extension provides several runtime parameters that can be changed at the session level:

| Parameter | Type | Default | Description |
| :--- | :---: | :---: | :--- |
| `memleak_analyzer.rollback_mode` | `bool` | `true` | Wrap analyzed queries in a subtransaction and rollback after execution. |
| `memleak_analyzer.max_context_level_displayed` | `int` | `-1` | Maximum depth level of memory contexts to display (-1 for unlimited). |
| `memleak_analyzer.merge_contexts` | `bool` | `false` | Merge memory contexts with identical names and parent paths into a single node. |
| `memleak_analyzer.show_positive_deltas_only` | `bool` | `false` | Display only contexts that have non-zero memory delta. |
| `memleak_analyzer.enable_warmup` | `bool` | `true` | Perform a warmup execution phase before profiled execution. |

---

## API reference

### 1. Profiling Client Queries

Profile any arbitrary SQL statement within your current session:

```sql
SELECT * FROM memleak_analyzer.analyze_query('SELECT * FROM my_function();');
```

### 2. Obtaining a Single BGW Snapshot

Get a single snapshot of a background worker's memory tree:

```sql
SELECT * FROM memleak_analyzer.get_bgw_snapshot(69420); -- Replace 69420 with target BGW PID
```

### 3. Profiling BGW Memory Over Time

Profile background worker's memory changes over a specific observation window (in seconds):

```sql
SELECT * FROM memleak_analyzer.analyze_bgw(69420, 42); -- Profile PID 69420 over a 42-second interval
```

---

## Regression Testing

To run the automated test suite under the standard `pg_regress` framework:

```bash
make installcheck
```