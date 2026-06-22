# LRU-K Page Replacement Simulator

A compact, reproducible Python simulator for the **LRU-K page replacement algorithm**, inspired by:

> Elizabeth J. O'Neil, Patrick E. O'Neil, and Gerhard Weikum,  
> *The LRU-K Page Replacement Algorithm for Database Disk Buffering*, SIGMOD 1993.

This project implements LRU-K with a configurable `K` value, includes LRU and LFU baselines, and provides paper-style synthetic experiments for evaluating page replacement behavior. The OLTP experiment from the paper is intentionally omitted because it depends on a legacy/system-specific setup that is not reproducible in a portable standalone simulator.

---

## Contents

- [Overview](#overview)
- [Implemented Algorithms](#implemented-algorithms)
- [Files](#files)
- [Requirements](#requirements)
- [Quick Start](#quick-start)
- [Running Experiments](#running-experiments)
- [Using LRU-K in Your Own Code](#using-lru-k-in-your-own-code)
- [Experiment Descriptions](#experiment-descriptions)
- [Testing](#testing)
- [Important Parameters](#important-parameters)
- [Implementation Notes](#implementation-notes)
- [Limitations](#limitations)

---

## Overview

LRU-K is a buffer/page replacement policy that improves upon classical LRU by tracking the last `K` references to each page. Instead of evicting the page with the oldest most recent reference, LRU-K evicts the page with the largest **backward K-distance**.

For a page `p`, the backward K-distance is the distance from the current logical time to the K-th most recent reference to `p`.

- If a page has been referenced at least `K` times, its backward K-distance is finite.
- If a page has fewer than `K` retained references, its backward K-distance is treated as infinity.
- When multiple pages have equal distance, including infinity, this simulator breaks ties using classical LRU behavior.

This helps distinguish frequently reused pages from pages that were touched only recently but are unlikely to be reused.

---

## Implemented Algorithms

The simulator includes:

1. **LRUKCache**
   - Configurable `K`
   - Optional correlated-reference filtering
   - Optional retained histories for nonresident pages
   - LRU tie-breaking

2. **LRUCache**
   - Classical least-recently-used replacement

3. **LFUCache**
   - Simple least-frequently-used replacement
   - Ties are broken by LRU among pages with the same frequency

---

## Files

| File | Description |
|---|---|
| `lruk_sim.py` | Main simulator, workload generators, CLI, and tests |
| `README.md` | This documentation file |

---

## Requirements

The simulator uses only the Python standard library.

Recommended Python version:

```bash
python 3.9+
```

No external packages are required.

---

## Quick Start

Run the default B-tree-style experiment:

```bash
python lruk_sim.py --experiment btree --capacity 101 --k 2
```

Run all included tests:

```bash
python lruk_sim.py --experiment tests
```

Run the sequential scan flooding experiment:

```bash
python lruk_sim.py --experiment scan --capacity 1000 --k 2
```

Try a different K value:

```bash
python lruk_sim.py --experiment scan --capacity 1000 --k 3
```

Use correlated-reference filtering:

```bash
python lruk_sim.py --experiment correlated --capacity 250 --k 2 --correlation-window 3
```

---

## Running Experiments

The command-line interface supports the following experiments:

```bash
python lruk_sim.py --experiment <name> --capacity <frames> --k <K>
```

Available experiment names:

| Experiment | Description |
|---|---|
| `btree` | B-tree index/data-page discrimination workload |
| `scan` | Sequential-scan flooding workload |
| `stable` | Stable independent-reference unequal-popularity workload |
| `changing` | Changing hot-set/adaptation workload |
| `correlated` | Correlated-reference burst workload |
| `tests` | Runs the included unit tests |

Example:

```bash
python lruk_sim.py --experiment stable --capacity 250 --k 2
```

Example output format:

```text
experiment=stable capacity=250 k=2
     LRU: accesses=200000 hits=... misses=... hit_rate=...
     LFU: accesses=200000 hits=... misses=... hit_rate=...
   LRU-2: accesses=200000 hits=... misses=... hit_rate=...
```

Exact values may differ if workload parameters or random seeds are changed.

---

## Using LRU-K in Your Own Code

You can import the simulator classes and workload generators directly.

```python
from lruk_sim import LRUKCache, run, btree_index_data_workload

cache = LRUKCache(capacity=101, k=2)
workload = btree_index_data_workload(n_transactions=100_000)

stats = run(cache, workload)

print("Accesses:", stats.accesses)
print("Hits:", stats.hits)
print("Misses:", stats.misses)
print("Hit rate:", stats.hit_rate)
print("Miss rate:", stats.miss_rate)
```

You can also feed your own page reference stream:

```python
from lruk_sim import LRUKCache

references = ["A", "B", "A", "C", "A", "B", "D", "A"]
cache = LRUKCache(capacity=2, k=2)

for page in references:
    hit = cache.access(page)
    print(page, "hit" if hit else "miss")

print(cache.stats)
```

---

## Experiment Descriptions

### 1. B-tree Index/Data-Page Discrimination

Function:

```python
btree_index_data_workload(...)
```

This workload models a transaction pattern where each transaction accesses:

1. One random B-tree index leaf page
2. One random data page

Index pages are selected from a much smaller page set, so index pages are reused more frequently. Data pages are selected from a much larger page set, so individual data pages are reused much less frequently.

This experiment demonstrates how LRU-K can retain frequently reused index pages better than classical LRU.

---

### 2. Sequential Scan Flooding

Function:

```python
sequential_scan_flooding_workload(...)
```

This workload mixes:

- A hot shared page set that receives most references
- A large sequential scan over cold pages

Classical LRU can be vulnerable to scan flooding because sequentially scanned pages may displace useful hot pages. LRU-K is designed to identify that scan pages lack enough repeated references and should not be retained as aggressively.

---

### 3. Stable Independent-Reference Workload

Function:

```python
stable_independent_reference_workload(...)
```

This workload generates references from two groups:

- A smaller hot set
- A larger cold set

The hot set receives a larger fraction of requests. This provides a stable popularity distribution where LRU-K should benefit from additional reference-history information.

---

### 4. Changing Hot-Set / Adaptation Workload

Function:

```python
changing_hotset_workload(...)
```

This workload has two phases:

1. Phase A: one set of pages is hot
2. Phase B: a different set of pages becomes hot

The experiment is useful for comparing LRU-K against LFU. LFU can retain pages that were historically popular even after access patterns change, while LRU-K considers only the last `K` retained references and therefore has a built-in aging effect.

---

### 5. Correlated-Reference Burst Workload

Function:

```python
correlated_burst_workload(...)
```

This workload emits short repeated bursts to the same page.

The `correlation_window` parameter can be used to prevent immediate repeated references from all counting as independent evidence of long-term popularity.

Example:

```bash
python lruk_sim.py --experiment correlated --capacity 250 --k 2 --correlation-window 3
```

---

## Testing

Run all included tests:

```bash
python lruk_sim.py --experiment tests
```

The tests cover:

- Configurable `K`
- B-tree-style workload behavior
- Sequential scan flooding behavior
- Stable independent-reference behavior
- Changing hot-set behavior
- Correlated-reference filtering execution

The tests are designed as sanity checks for the simulator and synthetic workloads. They are not intended to exactly reproduce every numerical result from the original paper.

---

## Important Parameters

### `capacity`

Number of buffer frames/pages that the cache can hold.

```python
LRUKCache(capacity=100, k=2)
```

### `k`

The number of retained references used by LRU-K.

```python
LRUKCache(capacity=100, k=3)
```

- `k=1` approximates classical LRU behavior.
- `k=2` is the most common LRU-K configuration discussed in the paper.
- Larger `k` values may be more stable for long-running stationary workloads but can adapt more slowly when access patterns change.

### `correlation_window`

Logical time window used to filter correlated references.

```python
LRUKCache(capacity=100, k=2, correlation_window=3)
```

If a page is referenced again within fewer than `correlation_window` logical accesses since its last retained reference, that repeated reference is not added to the page's K-history.

### `retain_history`

Controls whether page-reference histories are retained after a page is evicted.

```python
LRUKCache(capacity=100, k=2, retain_history=True)
```

Retaining history allows LRU-K to recognize pages that were recently evicted but have meaningful historical reference information.

### `max_history_pages`

Optional cap on the number of retained page histories.

```python
LRUKCache(capacity=100, k=2, max_history_pages=10000)
```

This is useful when simulating very large page universes.

---

## Implementation Notes

### Victim Selection

When the cache is full and a new page must be loaded, LRU-K chooses the resident page with the largest backward K-distance.

Pages with fewer than `K` references have infinite backward K-distance and are preferred as eviction candidates. If multiple pages have infinite distance, the simulator evicts the least recently used among those pages.

### Logical Time

The simulator advances logical time by one on every page access. Time is measured in page-reference counts, not wall-clock seconds.

### Histories

Each page stores at most `K` retained reference timestamps in a deque. By default, page histories are retained even after eviction.

### LFU Baseline

The LFU baseline is intentionally simple. It counts all historical references and uses LRU tie-breaking among pages with the same frequency.

---

## Limitations

- The simulator is intended for education, experimentation, and algorithm comparison.
- The included workloads are synthetic and deterministic under fixed seeds.
- The OLTP experiment from the original paper is skipped because it is not portable or reproducible without the original legacy environment.
- The code prioritizes clarity over highly optimized data structures. Victim selection scans resident pages, which is acceptable for small and medium simulations but may be slow for very large cache sizes.
- The paper's exact numeric results may not match this simulator because the original experiments used specific simulation assumptions and, in some cases, system-dependent workloads.

---

## Suggested Next Steps

Possible extensions:

- Add CSV export for experiment results
- Add matplotlib plots for hit rate versus buffer capacity
- Implement an optimized priority-queue-based victim selector
- Add more replacement policies such as 2Q, ARC, CLOCK, or GCLOCK
- Add real trace-file input support

