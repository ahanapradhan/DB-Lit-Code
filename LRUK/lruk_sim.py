
"""
lruk_sim.py

A small, reproducible LRU-K simulator inspired by:
  O'Neil, O'Neil, Weikum. "The LRU-K Page Replacement Algorithm for Database Disk Buffering." SIGMOD 1993.

What this file includes:
  - LRU-K with configurable K
  - Optional correlated-reference filtering via correlation_window
  - LRU and LFU baselines
  - Synthetic workload generators matching the paper's reproducible simulation themes:
      1) B-tree index/data-page discrimination
      2) Sequential-scan flooding
      3) Stable independent-reference unequal-popularity workload
      4) Changing hot set / adaptation workload
      5) Correlated-reference burst workload
  - Unit tests for the experiments above

The paper's OLTP experiment is intentionally omitted because it depends on a legacy/system-specific setup.
"""
from __future__ import annotations

from collections import OrderedDict, defaultdict, deque
from dataclasses import dataclass
import argparse
import random
import unittest
from typing import Deque, Dict, Hashable, Iterable, Iterator, List, Optional, Tuple

Page = Hashable


@dataclass
class Stats:
    accesses: int = 0
    hits: int = 0
    misses: int = 0

    @property
    def hit_rate(self) -> float:
        return self.hits / self.accesses if self.accesses else 0.0

    @property
    def miss_rate(self) -> float:
        return self.misses / self.accesses if self.accesses else 0.0


class ReplacementPolicy:
    def access(self, page: Page) -> bool:
        raise NotImplementedError

    @property
    def stats(self) -> Stats:
        raise NotImplementedError


class LRUCache(ReplacementPolicy):
    """Classical LRU baseline."""
    def __init__(self, capacity: int):
        if capacity < 0:
            raise ValueError("capacity must be non-negative")
        self.capacity = capacity
        self.od: OrderedDict[Page, None] = OrderedDict()
        self._stats = Stats()

    @property
    def stats(self) -> Stats:
        return self._stats

    def access(self, page: Page) -> bool:
        self._stats.accesses += 1
        hit = page in self.od
        if hit:
            self._stats.hits += 1
            self.od.move_to_end(page)
            return True
        self._stats.misses += 1
        if self.capacity == 0:
            return False
        if len(self.od) >= self.capacity:
            self.od.popitem(last=False)
        self.od[page] = None
        return False


class LFUCache(ReplacementPolicy):
    """Simple LFU baseline. Ties are broken by LRU among same frequency."""
    def __init__(self, capacity: int):
        if capacity < 0:
            raise ValueError("capacity must be non-negative")
        self.capacity = capacity
        self.cache: set[Page] = set()
        self.freq: Dict[Page, int] = defaultdict(int)
        self.last_access: Dict[Page, int] = {}
        self.t = 0
        self._stats = Stats()

    @property
    def stats(self) -> Stats:
        return self._stats

    def access(self, page: Page) -> bool:
        self.t += 1
        self._stats.accesses += 1
        self.freq[page] += 1
        hit = page in self.cache
        if hit:
            self._stats.hits += 1
            self.last_access[page] = self.t
            return True
        self._stats.misses += 1
        if self.capacity == 0:
            return False
        if len(self.cache) >= self.capacity:
            victim = min(self.cache, key=lambda p: (self.freq[p], self.last_access.get(p, -1)))
            self.cache.remove(victim)
        self.cache.add(page)
        self.last_access[page] = self.t
        return False


class LRUKCache(ReplacementPolicy):
    """
    LRU-K page replacement.

    Victim rule:
      - For each resident page p, compute backward K-distance = current_time - time_of_Kth_most_recent_reference.
      - If p has fewer than K retained references, distance is infinity.
      - Evict the resident page with maximum backward K-distance.
      - Ties, including infinity ties, are broken by classical LRU.

    Parameters:
      capacity: number of buffer frames.
      k: K in LRU-K. k=1 behaves like LRU with retained-history tie machinery.
      correlation_window: if >0, references occurring fewer than this many logical time ticks
        after the previous retained reference to the same page do not count toward the K-history.
        This models the paper's correlated-reference concern using logical access counts.
      retain_history: if True, histories are retained for pages after eviction.
      max_history_pages: optional cap for nonresident page histories; oldest histories are dropped.
    """
    def __init__(
        self,
        capacity: int,
        k: int = 2,
        correlation_window: int = 0,
        retain_history: bool = True,
        max_history_pages: Optional[int] = None,
    ):
        if capacity < 0:
            raise ValueError("capacity must be non-negative")
        if k < 1:
            raise ValueError("k must be >= 1")
        if correlation_window < 0:
            raise ValueError("correlation_window must be >= 0")
        self.capacity = capacity
        self.k = k
        self.correlation_window = correlation_window
        self.retain_history = retain_history
        self.max_history_pages = max_history_pages

        self.cache: set[Page] = set()
        self.history: Dict[Page, Deque[int]] = {}
        self.history_lru: OrderedDict[Page, None] = OrderedDict()
        self.last_access: Dict[Page, int] = {}
        self.t = 0
        self._stats = Stats()

    @property
    def stats(self) -> Stats:
        return self._stats

    def _get_hist(self, page: Page) -> Deque[int]:
        h = self.history.get(page)
        if h is None:
            h = deque(maxlen=self.k)
            self.history[page] = h
        self.history_lru[page] = None
        self.history_lru.move_to_end(page)
        return h

    def _maybe_trim_history(self) -> None:
        if self.max_history_pages is None:
            return
        while len(self.history_lru) > self.max_history_pages:
            old, _ = self.history_lru.popitem(last=False)
            if old not in self.cache:
                self.history.pop(old, None)
            else:
                # Keep resident-page history; move it back and stop to avoid deleting resident data.
                self.history_lru[old] = None
                self.history_lru.move_to_end(old)
                break

    def _record_reference(self, page: Page) -> None:
        h = self._get_hist(page)
        if not h or (self.t - h[-1]) >= self.correlation_window:
            h.append(self.t)
        self._maybe_trim_history()

    def _backward_k_distance(self, page: Page) -> float:
        h = self.history.get(page)
        if h is None or len(h) < self.k:
            return float("inf")
        return self.t - h[0]

    def _victim(self) -> Page:
        # Max distance wins. For infinity/finite ties, evict least recently used.
        def score(p: Page) -> Tuple[int, float, int]:
            d = self._backward_k_distance(p)
            is_inf = 1 if d == float("inf") else 0
            return (is_inf, d if d != float("inf") else 0.0, -self.last_access.get(p, -1))
        return max(self.cache, key=score)

    def access(self, page: Page) -> bool:
        self.t += 1
        self._stats.accesses += 1
        hit = page in self.cache
        if hit:
            self._stats.hits += 1
            self.last_access[page] = self.t
            self._record_reference(page)
            return True

        self._stats.misses += 1
        if self.capacity == 0:
            self._record_reference(page)
            return False

        if len(self.cache) >= self.capacity:
            victim = self._victim()
            self.cache.remove(victim)
            if not self.retain_history:
                self.history.pop(victim, None)
                self.history_lru.pop(victim, None)

        self.cache.add(page)
        self.last_access[page] = self.t
        self._record_reference(page)
        return False


# ------------------------- workload generators -------------------------

def btree_index_data_workload(
    n_transactions: int = 100_000,
    index_pages: int = 100,
    data_pages: int = 10_000,
    seed: int = 1,
) -> Iterator[str]:
    """
    Paper-style B-tree example: each transaction touches one random index leaf page
    and one random data page. Index pages are much more frequently reused.
    """
    rng = random.Random(seed)
    for _ in range(n_transactions):
        yield f"I{rng.randrange(index_pages)}"
        yield f"D{rng.randrange(data_pages)}"


def sequential_scan_flooding_workload(
    n_refs: int = 200_000,
    hot_pages: int = 1_000,
    cold_scan_pages: int = 50_000,
    hot_probability: float = 0.95,
    seed: int = 2,
) -> Iterator[str]:
    """
    Paper-style sequential flooding: most references come from a hot shared set,
    while the remaining references walk sequentially through a large cold relation.
    """
    rng = random.Random(seed)
    scan_pos = 0
    for _ in range(n_refs):
        if rng.random() < hot_probability:
            yield f"H{rng.randrange(hot_pages)}"
        else:
            yield f"S{scan_pos % cold_scan_pages}"
            scan_pos += 1


def stable_independent_reference_workload(
    n_refs: int = 200_000,
    hot_pages: int = 200,
    cold_pages: int = 20_000,
    hot_probability: float = 0.80,
    seed: int = 3,
) -> Iterator[str]:
    """Stable independent-reference workload with unequal page popularities."""
    rng = random.Random(seed)
    for _ in range(n_refs):
        if rng.random() < hot_probability:
            yield f"H{rng.randrange(hot_pages)}"
        else:
            yield f"C{rng.randrange(cold_pages)}"


def changing_hotset_workload(
    phase_refs: int = 100_000,
    hot_pages: int = 500,
    cold_pages: int = 20_000,
    hot_probability: float = 0.90,
    seed: int = 4,
) -> Iterator[str]:
    """
    Two-phase workload where the hot set changes. Useful for observing LRU-K aging
    versus LFU's long memory.
    """
    rng = random.Random(seed)
    for phase in [0, 1]:
        prefix = "A" if phase == 0 else "B"
        for _ in range(phase_refs):
            if rng.random() < hot_probability:
                yield f"{prefix}{rng.randrange(hot_pages)}"
            else:
                yield f"C{rng.randrange(cold_pages)}"


def correlated_burst_workload(
    logical_refs: int = 50_000,
    hot_pages: int = 200,
    cold_pages: int = 10_000,
    hot_probability: float = 0.80,
    burst_len: int = 4,
    seed: int = 5,
) -> Iterator[str]:
    """
    Emits repeated immediate references to the same page. With correlation_window > 1,
    those immediate repeats are filtered out of the K-history, while still counting as cache accesses.
    """
    rng = random.Random(seed)
    for _ in range(logical_refs):
        if rng.random() < hot_probability:
            p = f"H{rng.randrange(hot_pages)}"
        else:
            p = f"C{rng.randrange(cold_pages)}"
        for _ in range(burst_len):
            yield p


# ------------------------- simulation helpers -------------------------

def run(policy: ReplacementPolicy, workload: Iterable[Page]) -> Stats:
    for p in workload:
        policy.access(p)
    return policy.stats


def compare_policies(workload_factory, capacity: int, k: int = 2, correlation_window: int = 0) -> Dict[str, Stats]:
    policies = {
        "LRU": LRUCache(capacity),
        "LFU": LFUCache(capacity),
        f"LRU-{k}": LRUKCache(capacity, k=k, correlation_window=correlation_window),
    }
    out: Dict[str, Stats] = {}
    for name, policy in policies.items():
        out[name] = run(policy, workload_factory())
    return out


# ------------------------- tests / experiments -------------------------

class LRUKExperimentTests(unittest.TestCase):
    def test_k_is_input_parameter_and_basic_hits(self):
        c = LRUKCache(capacity=2, k=3)
        for p in ["A", "B", "A", "C", "A", "B", "A"]:
            c.access(p)
        self.assertEqual(c.k, 3)
        self.assertGreaterEqual(c.stats.accesses, 7)

    def test_btree_index_data_experiment_lruk_beats_lru(self):
        cap = 101
        n = 60_000
        lru = run(LRUCache(cap), btree_index_data_workload(n_transactions=n, seed=10))
        lruk = run(LRUKCache(cap, k=2), btree_index_data_workload(n_transactions=n, seed=10))
        self.assertGreater(lruk.hit_rate, lru.hit_rate)

    def test_sequential_scan_flooding_experiment_lruk_beats_lru(self):
        cap = 1_000
        lru = run(LRUCache(cap), sequential_scan_flooding_workload(n_refs=120_000, seed=11))
        lruk = run(LRUKCache(cap, k=2), sequential_scan_flooding_workload(n_refs=120_000, seed=11))
        self.assertGreater(lruk.hit_rate, lru.hit_rate)

    def test_stable_independent_reference_experiment_lruk_beats_lru(self):
        cap = 250
        lru = run(LRUCache(cap), stable_independent_reference_workload(n_refs=120_000, seed=12))
        lruk = run(LRUKCache(cap, k=2), stable_independent_reference_workload(n_refs=120_000, seed=12))
        self.assertGreater(lruk.hit_rate, lru.hit_rate)

    def test_changing_hotset_experiment_lruk_adapts_better_than_lfu(self):
        cap = 500
        lfu = run(LFUCache(cap), changing_hotset_workload(phase_refs=80_000, seed=13))
        lruk = run(LRUKCache(cap, k=2), changing_hotset_workload(phase_refs=80_000, seed=13))
        self.assertGreater(lruk.hit_rate, lfu.hit_rate)

    def test_correlated_burst_experiment_filter_runs(self):
        cap = 250
        no_filter = run(LRUKCache(cap, k=2, correlation_window=0), correlated_burst_workload(seed=14))
        filtered = run(LRUKCache(cap, k=2, correlation_window=3), correlated_burst_workload(seed=14))
        self.assertEqual(no_filter.accesses, filtered.accesses)
        self.assertGreater(no_filter.hit_rate, 0.0)
        self.assertGreater(filtered.hit_rate, 0.0)


def main() -> None:
    parser = argparse.ArgumentParser(description="LRU-K simulator with paper-style synthetic experiments")
    parser.add_argument("--experiment", choices=["btree", "scan", "stable", "changing", "correlated", "tests"], default="btree")
    parser.add_argument("--capacity", type=int, default=101)
    parser.add_argument("--k", type=int, default=2)
    parser.add_argument("--correlation-window", type=int, default=0)
    args = parser.parse_args()

    if args.experiment == "tests":
        unittest.main(argv=["ignored"], exit=False)
        return

    factories = {
        "btree": lambda: btree_index_data_workload(),
        "scan": lambda: sequential_scan_flooding_workload(),
        "stable": lambda: stable_independent_reference_workload(),
        "changing": lambda: changing_hotset_workload(),
        "correlated": lambda: correlated_burst_workload(),
    }
    results = compare_policies(
        factories[args.experiment],
        capacity=args.capacity,
        k=args.k,
        correlation_window=args.correlation_window,
    )
    print(f"experiment={args.experiment} capacity={args.capacity} k={args.k}")
    for name, st in results.items():
        print(f"{name:>8s}: accesses={st.accesses} hits={st.hits} misses={st.misses} hit_rate={st.hit_rate:.4f}")


if __name__ == "__main__":
    main()
