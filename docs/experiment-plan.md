# Concurrent Inverted Index — Experiment Plan

Nov 15, 2026 · @Pietro Bertozzi

## Purpose

This plan fixes which questions the benchmark answers and which concurrent variants are built to answer them: five questions, four core variants in all four languages (16 implementations), plus up to three optional extras.

It complements `docs/design.md`, which fixes the shared infrastructure (workload, data, CLI contract, results, methodology). Once agreed, the questions and the variant list move into `design.md` as a new section, and this plan becomes the report's backbone: each question below is meant to be one chapter of the results.

**Time limit.** Every experiment (one params file per question) must take about one hour, builds included, and at most 70 minutes estimated (docs/design.md, Run conditions). The experiments below are therefore small, targeted sweeps, not one big sweep; `bench/params-q1.sh` is the first.

Status, 2026-11-16: every variant of the plan is implemented in its planned languages and passes `bench/check.sh`: `global-lock`, `partition-lock`, `private-merge`, `private-merge-heap` and `partition-writers` in all four languages, `private-merge-static` in C, `partition-rwlock` in Java and C. All the experiments have run (Q1 to Q5 and the Q2 follow-up Q2b) and each question has a Results section below its predictions. The question the first Q2 sweep left open, how `private-merge`'s N-way merge is implemented and whether it decides the answer, is answered by Q2b. The plots are done (`analysis/make_figures.py`); the presentation is the final deliverable. The repo copy, `docs/experiment-plan.md`, is merged with the exported one after each round.

## The problem space

A concurrent inverted index can vary along four groups of dimensions; this plan explores the structural ones that matter most, fixes most of the rest, and leaves some out on purpose.

An inverted index is a map from each term to its posting list (the documents containing it, with a frequency). Building it concurrently means many workers turn documents into postings and add them to shared lists. Every design choice below is an answer to one problem: two workers adding to the same list, or the same map, at the same time.

### Structural dimensions (change the code)

| Dimension | Options | What it decides | In this plan |
| --- | --- | --- | --- |
| Partitioning | single · by term · by document | How the index is split: one map; terms hashed to P sub-maps; one private index per worker | all three |
| Writer | direct · central writer · writer per partition | Who writes: every worker itself, or owner threads fed through queues | direct, writer per partition |
| Coordination | lock · read/write lock · CAS / lock-free | How concurrent access is made safe | lock; read/write lock as an extra; CAS left out |
| Granularity | whole index · partition · posting list | How much one lock protects | index, partition |
| Batching | 1 · 64 · 1024 postings per lock or message | How often a worker synchronizes | yes (`BATCH`) |
| Work distribution | static split · dynamic handout | Whether documents are assigned up front or taken from a shared counter | dynamic everywhere; static as one side experiment |
| Write timing | immediate · deferred | Whether postings land in the index at once or are buffered and merged | immediate; deferred in `private-merge` |
| Visibility | partial · snapshot per document · snapshot per batch | What a query may see while documents are being inserted | partial |

### Each structural dimension, explained

Building an index is two jobs: **tokenizing** (splitting a document into words and counting them) and **writing** (adding each word's posting to its list). Tokenizing is private to each worker; writing touches shared data, and that is where every problem starts.

The basic danger is a **race condition**. Two workers append to the list of "the" at the same moment:

```
worker A: reads length = 100      worker B: reads length = 100
worker A: writes doc 7 at slot 100
                                  worker B: writes doc 9 at slot 100   (doc 7 is lost)
```

The result is fast and wrong: a document silently missing from the index. This is the bug `bench/check.sh` exists to catch. The eight dimensions below are eight ways of preventing it, or of paying less for preventing it.

#### 1. Partitioning: how the index is split

What is partitioned is the index itself, the map from each word to its posting list.

- **Single.** One map holds every word; all workers share it.
- **By term.** The words are spread over P separate maps. A fixed rule, hash(word) mod P, decides where each word lives, so "dog" is always in the same map. Two workers only collide when their words live in the same map.
- **By document.** The documents are split instead. Each worker builds its own complete, private map from the documents it processed, so building needs no synchronization at all. At the end the private maps are merged into one.

```
single:       "cat" → [0, 1]   "dog" → [1]   "the" → [0, 1, 2]        (one map)
by term:      map 0: "cat", "sat"   map 1: "dog", "the"   map 2: ...   (P maps)
by document:  worker 0: "the" → [0, 2, 4]   worker 1: "the" → [1, 3, 5]
              merged at the end: "the" → [0, 1, 2, 3, 4, 5]
```

The catch of by-term partitioning is the **hot partition**: "the" appears in almost every document, so whichever map holds it is busy all the time (Q1). The catch of by-document partitioning is the **merge** (Q2).

#### 2. Writer: who writes into the index

- **Direct.** Every worker does both jobs: it tokenizes a document, then writes the postings itself. Several workers write into shared data, so they need locks. All lock variants are direct.
- **Central writer.** Workers only tokenize. They put their postings in a queue, and one single thread takes them out and inserts them. Only one thread ever writes, so the index needs no lock, but that thread does all the writing alone and becomes the bottleneck.
- **Writer per partition.** Combined with by-term partitioning: each of the P maps has its own writer thread, the only one allowed to touch it. A worker sends each posting to the writer that owns its word.

The idea behind it: "direct" protects shared data with **locks** (everyone may write, taking turns); the writer options avoid sharing: each piece of data has **one owner**, and everyone else sends it **messages** (Q3).

In `partition-writers` the owners are not extra threads: the workers themselves own the partitions, so the variant uses exactly N threads like every other.

#### 3. Coordination: the mechanism that prevents collisions

- **Lock (mutex).** A thread takes the lock before touching the data; anyone arriving later waits until it is released. Simple and safe, but a waiting thread does nothing.
- **Read/write lock.** A smarter lock that tells readers from writers: any number of readers may be inside together, because reading breaks nothing; a writer must be alone. It pays off when queries far outnumber inserts, and costs a little more to take than a plain lock (Q5).
- **CAS / lock-free.** No lock at all: a special CPU instruction, *compare-and-swap*, writes a new value only if the current one is still the one the thread read; otherwise it retries. Nobody ever waits, but it is very hard to write correctly for a growing hash map, and a subtle mistake gives fast, wrong numbers. Left out.

```
lock:  lock(m); list_the.append(doc 7); unlock(m)
CAS:   do { old = length } while (!CAS(&length, old, old + 1))
```

With writer threads, coordination does not disappear: it moves to the **queue**, which worker and writer share and which needs a lock or CAS of its own.

#### 4. Granularity: how much data one lock protects

- **Whole index.** One lock for the whole map. While worker A adds "cat", worker B cannot add "dog", even though the words are unrelated. This is `global-lock`.
- **Partition.** With by-term partitioning, one lock per map. Workers writing words in different maps work in parallel; more partitions, fewer collisions. This is `partition-lock`.
- **Posting list.** One lock per word: workers collide only on exactly the same word. It looks best, but millions of words mean millions of locks, and adding a *new* word changes the map itself, which then needs a second mechanism. Cut for that reason.

|  | Coarse locks (few) | Fine locks (many) |
| --- | --- | --- |
| Collisions between threads | many | few |
| Cost and complexity | low | high |

No granularity removes the limit of "the": the lock that protects it is always busy.

#### 5. Batching: how often a worker synchronizes

Taking a lock has a fixed cost: the lock's cache line must travel from one core to another, which costs about as much as inserting one posting.

- **Batch 1.** One lock per posting. A document has about 180 distinct words (430 words in all), so about 180 lock acquisitions per document; half the time goes to taking and releasing locks.
- **Batch 1024.** The worker collects postings in a private buffer (no lock), across documents, and inserts 1024 at a time under one acquisition. The fixed cost is divided by 1024.

```
batch 1:     for each word: lock; add 1 posting; unlock
batch 1024:  buffer postings locally; when 1024: lock; add all; unlock
```

Like shopping: one trip per item, or one trip with the whole list. The trade-off is that a large batch holds the lock longer, so others wait longer each time. With messages it is the same: one message per posting, or one message carrying 1024. Q1 predicts that batching matters more than granularity.

#### 6. Work distribution: which worker processes which document

- **Static split.** The corpus is cut into N equal ranges before starting (with 4 workers and 100,000 documents, worker 0 takes 0–24,999, and so on). Simple: workers never talk to each other.
- **Dynamic handout.** A shared counter holds the next document to process. A worker that finishes a document takes the next one (`d = counter++`, atomic). Faster workers simply take more documents.

On the 155H this matters a lot, because its cores run at different speeds. With a static split every worker gets the same amount of work, and the phase ends when the slowest one does:

```
static:   P-core     ████████ done
          E-core     ████████████████ done
          LP E-core  ████████████████████████ done   ← everyone waits for it
dynamic:  all cores  ████████████████ done together
```

The dynamic counter is itself shared, but it is touched once per document, not once per word, so it costs almost nothing. Every variant uses dynamic handout; `private-merge-static` exists only to show what a static split costs (Q4).

#### 7. Write timing: when postings reach the shared index

- **Immediate.** A worker writes its postings into the shared index as soon as it has them (after at most one batch). The index is always nearly up to date, and locks are needed throughout. `global-lock`, `partition-lock` and `partition-writers` are immediate.
- **Deferred.** A worker accumulates everything privately and brings it into the shared index only at the end, in a merge. No locks while building, but extra work at the end, and until then the shared index does not exist. This is `private-merge`.

It is batching taken to the extreme: batch 1 accumulates one posting, batch 1024 accumulates 1024, deferred accumulates everything. The more is deferred, the less synchronization, but the more work later and the later data becomes visible. That is why `private-merge` runs build and query modes, not mixed ones: an inserted document would not be findable until the final merge.

#### 8. Visibility: what a query may see during inserts

This matters only in mixed modes, where queries and inserts run at the same time. Suppose document 500 contains "cat" and "dog", and a worker is halfway through inserting it:

```
1. adds doc 500 to the list of "cat"   done
2. adds doc 500 to the list of "dog"   not yet
   ← a query AND cat dog arrives here: doc 500 is in "cat" but not in "dog", so it is not found
```

The query saw a **half-inserted document**. The options say how much of that is acceptable:

- **Partial.** Chosen for every variant: the simplest to implement, and the fastest; a query is only "slightly off" for documents arriving at that exact moment.
- **Snapshot per document.** A document becomes visible everywhere at once or nowhere. One way: a counter "documents visible up to", advanced only when a document is fully inserted, and queries ignore anything beyond it. More correct, and it costs.
- **Snapshot per batch.** The same per group of documents: cheaper coordination, but queries see new data later.

A related problem is **ordering**. With dynamic handout, worker A takes document 10 and worker B document 11; if B is faster, it writes first and the list becomes `[…, 11, 10]`. `AND` and `OR` walk two **sorted** lists side by side, so an unsorted list gives wrong results. **Decision:** every variant keeps posting lists sorted by inserting each document ID in its correct place. The right place is almost always at or next to the end, because IDs arrive nearly in order, so the cost is small.

#### The eight dimensions in one line each

| # | Dimension | The question it answers |
| --- | --- | --- |
| 1 | Partitioning | How is the index split? |
| 2 | Writer | Who writes into it? |
| 3 | Coordination | What mechanism prevents collisions? |
| 4 | Granularity | How much data does one lock protect? |
| 5 | Batching | How often does a worker synchronize? |
| 6 | Work distribution | How is the work divided among workers? |
| 7 | Write timing | Write now, or at the end? |
| 8 | Visibility | What may a query see during inserts? |

### Workload dimensions (fixed by design.md)

| Dimension | Options | In this plan |
| --- | --- | --- |
| Mode | build · query · mixed 95/5, 50/50, 20/80 | all five |
| Scaling | strong · weak | both |
| Corpus size | 128 MB to 2048 MB prefixes | all |
| Query type | single term · AND · OR | all three, fixed mix |
| Load model | closed loop · open loop | closed loop only |

- **Mode** is what the workers do: only insert (build), only query, or a fixed mix of both. A mix decides how often readers and writers meet.
- **Scaling.** Strong: the same total work, more workers, so time should fall. Weak: work grows with the workers, so time should stay flat.
- **Corpus size** tests whether a result holds as the index grows, not only on one size.
- **Query type.** `SIMPLE` reads one list; `AND` and `OR` merge two, so they cost more and hold a lock longer.
- **Load model.** Closed loop: each worker sends its next operation as soon as the last one ends, which measures maximum capacity. Open loop (operations arriving at a set rate, as from real users) was left out.

### Hardware dimensions

| Dimension | Options | In this plan |
| --- | --- | --- |
| Workers | 1 to 22 | `1 2 4 6 8 12 14 16 22` strong, `1 2 4 8 16` weak |
| Core type | P-cores (6, with HT) · E-cores (8) · LP E-cores (2) | unpinned main run + P-cores-only control |
| Placement | free scheduling · pinned | both |

- **Workers** is N, the number of threads doing the work; the list includes 6, 12, 14, 16 and 22, where a core type runs out.
- **Core type.** The CPU mixes fast and slow cores, so worker 7 adds less than worker 6 did (Q4).
- **Placement.** Unpinned, the operating system decides which core runs each thread; pinned, threads are restricted to the P-cores. Comparing the two shows which bends in a curve come from the hardware.

### Language and runtime

| Dimension | Options | In this plan |
| --- | --- | --- |
| Language | Java · Go · C++ · C | all four |
| Memory management | garbage collected (Java, Go) · manual (C++, C) | measured, not varied |
| Standard concurrency tools | `synchronized` / `ReentrantLock`, `sync.Mutex` + channels, `std::mutex`, pthreads | each language's own |

- **Language.** The same strategy is written four times, so differences between languages are visible strategy by strategy.
- **Memory management.** Java and Go free memory with a garbage collector, which runs its own threads on other cores; C and C++ free it by hand. This is measured, not changed.
- **Concurrency tools.** Each language uses its own standard lock and queue, because that is what a programmer in that language would use.

### Deliberately left out

- **Lock-free (CAS) structures.** Hardest to get right and to explain; a wrong one gives fast, meaningless numbers. Future work.
- **Central writer.** Its result is predictable: one writer caps the speedup at about 2×. `partition-writers` answers the same question better.
- **Open-loop load, ranked retrieval, on-disk indexes.** Different problems; see design.md, Scope and non-goals.

## The questions

All numbers in the Results sections come from `analysis/make_figures.py` (the figures, and `--numbers` for the values quoted in the text), computed as medians of the measured repetitions; memory uses the first measured repetition (design.md, Memory semantics). Where a number here differs from an earlier draft of this plan, the script's value is the one that stands.

Five questions, each tied to one concurrency principle, each with an experiment and a prediction written before any measurement; a sixth runs across all of them: does the answer depend on the language?

Writing predictions first is deliberate. A result that confirms a prediction shows the principle at work; a result that contradicts it is the most interesting part of the report, because it means a hidden cost exists and must be explained.

### Q1. Is the cost the lock itself, or how often you take it?

**The strategy in short.** A lock can slow workers down in two different ways, and the experiment has one knob for each. Turning the knobs one at a time shows which cost dominates.

The two suspects (picture one bathroom with a single key kept at reception):

- **Overhead:** the trip to reception to fetch the key. It is paid **every time** a lock is taken, even when nobody else wants it.
- **Contention:** the **queue** at the door, because someone else holds the key.

The two knobs:

- **Batch: how often the key is fetched.** Batch 1 takes the lock for every posting, batch 1024 once every 1024. It cuts the trips (overhead), but not the queue: the work done inside stays the same.
- **Partitions: how many bathrooms there are.** 4 partitions give 4 queues, 64 give 64. It splits the queue (contention), but not the trips: each worker takes a lock just as often.

One extra measurement: **1 worker.** With a single worker no queue can exist, so whatever a variant loses against `sequential` at 1 worker is pure overhead (plus the per-document counting, see Rules every variant follows).

How the result is read:

| If we see… | The main cost is… |
| --- | --- |
| a larger batch helps a lot, more partitions little | overhead (too many lock acquisitions) |
| more partitions help a lot, a larger batch little | contention (too much waiting) |
| both help | both, and the sizes of the two gains say which weighs more |

An independent check: **voluntary context switches** count how often a thread fell asleep because the lock was taken, that is, how often it queued. Many means contention; few, while still slow, means overhead.

In one sentence: two knobs, one per cost, with the 1-worker run and the context switches as cross-checks. The prediction below is that, at small batches, overhead is the main cost.

**Fundamentals.**

- **Amdahl's law.** If a fraction s of the work is serial, the speedup with any number of workers is at most 1 / s. Work done while holding a shared lock is serial: only one worker can be inside.
- **The cost of one acquisition.** Taking a lock that another core just held moves the lock's cache line between cores (cache coherence), which costs tens to hundreds of nanoseconds. Inserting one posting costs about the same. With one lock per posting, synchronization doubles the work.
- **Batching amortizes.** Taking the lock once for B postings divides the acquisition cost by B. The serial fraction stays, but the overhead around it shrinks.
- **Zipf's law.** Word frequency in text falls steeply with rank: "the" alone is about 6% of English tokens. A by-term partition is only as fast as its busiest partition, and the busiest is the one holding the most frequent words.

**Why it matters.** It separates two costs that look the same in a speedup curve: contention (waiting for the lock) and overhead (taking it). Reducing one does not fix the other.

**Experiment.** `global-lock` and `partition-lock` (P = 4, 64), each with `BATCH` = 1, 64, 1024. Build mode, strong scaling, 128 MB corpus, workers 1 2 4 6 8 12 16 22, 1 warm-up + 5 repetitions: `./bench/run.sh bench/params-q1.sh` (292 runs, estimated at 66 minutes, accepted: see design.md, Run conditions, for the calibration). Read speedup, CPU utilization and voluntary context switches (blocking). P = 16 and the 14-worker point are not in this experiment (they stay in `params.sh`).

**Prediction.**

- `global-lock` with batch 1 is slower than `sequential` already at 2 workers and gets worse: every posting pays a cache-line transfer.
- `global-lock` with batch 1024 peaks at 1.5–2×: tokenizing runs in parallel, inserting does not.
- `partition-lock` is capped by its hottest partition. The partition holding "the" receives about 6% of postings (an assumption, to be counted on the corpus) plus its 1/P share of the rest: about 30% at P = 4 and 7.5% at P = 64. That bounds the insert phase at about 3.4× and 13×: with P = 4 the curve flattens early, with P = 64 there is much more room, though the gain is far below the 16× ratio in lock count.
- Batching matters more than partition count: batch 1 to 64 improves more than P = 4 to 64, at least below the P = 4 ceiling. Above 6 workers, lock effects and the end of the P-cores mix; Q1's conclusions are drawn mainly up to 6 workers, and `private-merge` (Q2) gives the lock-free reference above that.

**What would surprise us.** `partition-lock` scaling beyond 13× would mean the hot-term bound is wrong, for example because tokenizing dominates the work.

#### Q1 results (sweep of 2026-11-15)

The answer to Q1: at batch 1 both costs matter, but as soon as the batch is reasonable, **contention dominates**: one lock serializes about half of the work, and only more locks (partitions) shorten the queue. This contradicts the prediction that batching matters more than partition count, and the reason is measured below.

The predictions above are kept as written before the measurement; this section records how each one fared.

**Run.** `results/q1/20261115-204137/`: 292 runs × (1 warm-up + 5), 58 minutes, no failures, performance profile, on AC, no swap. The largest coefficient of variation per point is 12% (Java), 9.3% (C), 5.8% (Go), 5.2% (C++), so differences under about 10% are not reliable. Speedup = median `build_ns` of the language's `sequential` / median of the variant.

| Point | Java | Go | C++ | C |
| --- | --- | --- | --- | --- |
| `global-lock` B1, 2 workers | 0.90 | 0.62 | 0.72 | 0.62 |
| `global-lock` B1, 22 workers | 0.59 | 0.48 | 0.22 | 0.15 |
| `global-lock` B1024, peak | 1.68 (4 w) | 1.29 (22 w) | 1.41 (2 w) | 1.36 (2 w) |
| `partition-lock` P4, peak | 2.82 | 2.22 | 2.97 | 2.70 |
| `partition-lock` P64, peak | 4.27 | 3.32 | 5.60 | 4.43 |
| Overhead at 1 worker, `global-lock` B1 | 1.39× | 2.0× | 1.37× | 1.23× |

**1. A lock per posting is worse than no parallelism: confirmed, more strongly than predicted.** `global-lock` with batch 1 is below `sequential` at every worker count in every language. In C and C++ it collapses to 0.15× and 0.22× at 22 workers while keeping 15–18 cores busy. The context switches explain it: about 7.7 million per repetition in C++. Threads spend their time falling asleep on the lock and being woken up; the busy cores are the operating system managing the waiting, not index work. Go is the exception: it does not collapse (0.48×) and has 10–60× fewer context switches, because its mutex parks a waiting goroutine inside the runtime without involving the operating system.

**2. The serial fraction, measured: confirmed.** `global-lock` with batch 1024, where taking the lock costs almost nothing, stops at 1.4–1.7× (Go is the outlier, still rising at 1.29× at 22 workers). Against its own 1-worker time the peak is about 2× in Java and 1.45× in C, so by Amdahl's law writing is about **50–70% of the work**. This was the number every other prediction rested on.

**3. Batching matters more than partition count: contradicted.** At 22 workers, going from 4 to 64 partitions gains as much as, or more than, going from batch 1 to batch 64 in three languages out of four (Java and Go against, C++ a tie within noise, only C for). The prediction assumed the main cost was *taking* the lock (overhead). Result 2 rules that out: even at batch 1024, where acquisitions are almost free, a single lock caps at about 1.5×. The real problem is that one lock serializes half of the work. Batching reduces the trips to the lock but not the queue; only more locks shorten the queue.

**4. The hot-partition bound: holds at P = 4, not the limit at P = 64.** At P = 4 the bound is about 3.4× and the measured peaks are 2.2–3.0×, which fits. At P = 64 the bound is about 13×, but the best speedup is 3.3–5.6× while 17–18 cores are busy at 22 workers. Something other than the hot lock limits it; the likely candidates are the hardware: shared memory bandwidth (inserting into a large hash map jumps randomly through RAM) and the slower E-cores. Q1 alone cannot tell; `private-merge` (Q2) can: if it also stops near 5× with no locks at all, the limit is the hardware.

**Two further observations.**

- **Batch 64 beats batch 1024 at high worker counts** (Java, Go, C; C++ the exception, with B1024 slightly ahead). A large batch holds the lock longer (1024 inserts in a row), so waiting threads wait longer; and 64 buffers of 1024 postings per worker no longer fit in the cache.
- **C at batch 1024 has many more context switches** than the other languages at the same point (13–47 thousand against 3–5 thousand). A hypothesis, not yet tested: memory allocation (`malloc`) under many threads.

Scaling beyond 13×, the surprise named above, did not happen: the maximum speedup is 5.6×.

### Q2. Is sharing worth it at all?

**The strategy in short.** In Q1 every worker writes into one shared index and pays for locks. Q2 asks what happens if nothing is shared: is it cheaper for each worker to work alone, even though everything must be combined at the end?

Picture ten people compiling a dictionary:

- **Share** (`partition-lock`): one notebook, and they take turns writing in it. Time is lost waiting.
- **Replicate** (`private-merge`): each has their own notebook and never waits. At the end all the notebooks are copied into one, and ten notebooks are needed, each with its own copy of "the".

The two prices compared:

|  | Share | Replicate |
| --- | --- | --- |
| While building | synchronization (locks, waiting) | none |
| At the end | nothing | the merge |
| Memory | one index | N indexes, the common vocabulary repeated |

How the experiment answers:

- **The two costs land in different columns.** `private-merge` records the lock-free build of the private indexes in `build_ns` and the merge in `finalize_ns`. So the result shows not only who wins, but where the time goes: how much the build saves and how much the merge gives back.
- **The comparison is against the champion.** The total (build + finalize) of `private-merge` is compared with the total of the best `partition-lock` configuration found in Q1, not an arbitrary one.
- **The knob is the number of workers.** As N grows, the private build gets faster (more people writing in parallel), but the merge combines more notebooks and memory grows with more copies of the vocabulary. The interesting point is where the two trends cross.
- **Strong and weak scaling say different things.** Strong (same corpus, more workers) shows who is faster, and up to how many workers. Weak (corpus growing with the workers) has no locks at all, so if its time does not stay flat, the limit is the hardware (memory shared by all cores), not synchronization.

How the result is read:

| If we see… | It means… |
| --- | --- |
| `private-merge` total below `partition-lock` total | replicating pays: the merge costs less than all the locking it saves |
| a very fast build but a huge finalize | the merge eats the gain |
| `partition-lock` wins | locks are cheap, and synchronizing costs less than copying |
| `private-merge` also stops scaling | the limit is the hardware (memory, core types), not locks |

**An extra role: `private-merge` closes Q1.** Above 6 workers, Q1 alone cannot tell whether a curve bends because of locks or because the P-cores run out. `private-merge` has no locks, so its curve shows what the hardware allows by itself. If it bends at 6 too, the bend is the hardware; if it keeps rising while `partition-lock` stops, the bend is the locks.

In one sentence: measure the price of sharing (locks) and of replicating (merge and memory) separately, compare totals against the best `partition-lock`, and use the lock-free curve as the hardware reference for every variant.

**Fundamentals.**

- **The cheapest synchronization is none.** If each worker builds a private index from its own documents, building needs no locks, no shared cache lines and no waiting.
- **The cost moves, it does not vanish.** Private indexes must be merged into one, term by term. The merge is extra work that `sequential` never does, and if one thread merges, it is the new serial fraction.
- **Memory.** Each private index repeats the common vocabulary (every worker sees "the"), so peak memory grows with N.
- **Memory bandwidth** is shared by all cores. A workload that moves a lot of data stops scaling when the memory bus saturates, even with no locks at all.

**Why it matters.** It is the classic trade-off of parallel computing: share and synchronize, or replicate and combine. Many real indexers (MapReduce, Lucene segments) choose the second.

**Experiment.** `private-merge` against the best `partition-lock` from Q1 (P = 64, batch 64) and `sequential`. Build mode, strong scaling on the 128 MB corpus and weak scaling from a 128 MB base (1, 2, 4, 8 workers), workers `1 2 4 6 8 12 16 22` for strong, 1 warm-up + 5 repetitions. Read `build_ns`, `finalize_ns` (the merge), `peak_rss_bytes`. The parameter file is `bench/params-q2.sh` (272 runs). The runner runs strong scaling on every size listed for weak scaling, so strong scaling ran on 128, 256, 512 and 1024 MB, not only on 128 MB as first planned (128 MB only would have been 104 runs and about 34 minutes). The estimate was 127 minutes against the 70-minute limit (see design.md, Experiment Q2); the file was run as it is, overnight, in 115 minutes (see the results below).

**Prediction.**

- `private-merge` has the best build time at every N above 1.
- The merge takes 10–25% of total time at 16 workers if it is parallel; with a single merging thread it caps speedup at 4–5×.
- Peak memory grows clearly with N, by more than the other variants.
- In weak scaling, time grows slowly even with no locks: memory bandwidth, not synchronization, is the limit.

**What would surprise us.** `partition-lock` beating `private-merge` would mean the merge is more expensive than all the locking it replaces.

#### Q2 results (sweep of 2026-11-16)

The answer to Q2: in this implementation, **sharing wins**. `partition-lock` beats `private-merge` on total time at every worker count from 2 up, in all four languages; this is the outcome the plan named as its surprise. The lock-free build is faster, but the merge gives back more than it saves, because the merge takes about the same time whatever the number of workers.

**Run.** `results/q2/`, 272 runs × (1 + 5), 115 minutes (over the one-hour rule, accepted for the night), no failures, no swap. Strong scaling on 128, 256, 512 and 1024 MB; weak scaling 128 MB × N for N = 1 2 4 8. Speedup = sequential total / variant total (build + finalize). Baselines repeat within about 4% across sweeps, so differences under 5% are noise.

| 128 MB, strong | Java | Go | C++ | C |
| --- | --- | --- | --- | --- |
| `partition-lock` best speedup | 4.46 | 3.13 | 5.33 | 4.39 |
| `private-merge` best speedup | 3.53 | 2.51 | 3.06 | 2.26 |
| `private-merge` build only (no merge), 22 workers | 4.51 | 3.92 | 6.49 | 7.01 |
| merge share of total, 16 workers | 26% | 30% | 49% | 67% |
| merge time, 2 to 22 workers | 0.11–0.13 s | 0.13–0.24 s | 0.26–0.29 s | 0.26–0.32 s |
| peak memory, 22 workers: `private-merge` / `partition-lock` | 1.10 | 1.71 | 1.84 | 2.07 |

**1. Best build time at every N: contradicted on the total, confirmed on the build alone.** The lock-free build of `private-merge` is faster than `partition-lock` from 2 workers up in Go, C++ and C (up to 1.6× in C; a tie in Java). On build + merge, `partition-lock` is faster at every N ≥ 2 in all four languages (time ratio 0.47–0.90 at 128 MB). At 1024 MB the picture is the same, less sharp (Go ties up to 12 workers).

**2. The merge is 10–25% of the total: contradicted.** At 16 workers it is 26% (Java), 30% (Go), 49% (C++), 67% (C); only Java at 256–1024 MB falls in the range. The key observation: **the merge time is flat in the number of workers** (C: 0.26–0.32 s from 2 to 22 workers). The likely reason: with N workers each term appears in up to N private indexes, so the total merge work grows with N while the number of merging threads also grows with N, and the two cancel; merging is also mostly memory traffic (reading N lists, writing one), which more threads do not speed up. How the N-way merge is implemented (a linear scan of the N heads, N comparisons per posting) and whether a heap would change the picture is answered by the follow-up below (Q2b).

**3. Memory grows with N: confirmed for C, C++ and Go; unclear for Java.** At 128 MB, from 1 to 22 workers (first measured repetition, as design.md prescribes for the process-wide peak), `private-merge` grows 401 → 686 MB in C (+71%) and 321 → 594 MB in C++ (+85%), while `partition-lock` stays flat (324 → 332 and 310 → 323 MB); it uses up to 2.07× the memory of `partition-lock`. Go's curve is noisy (one reading per point). In Java both grow, and the JVM heap sizing hides the effect.

**4. Weak scaling: time grows without locks: confirmed; the cause is unclear.** For 8× the work, the lock-free build of `private-merge` takes 1.6–1.8× longer, and `partition-lock` about the same. Memory bandwidth is the natural suspect, but 8 workers already go beyond the 6 P-cores and no bandwidth counter was recorded, so it remains a hypothesis.

**Closing Q1.** `private-merge`'s build without its merge is the lock-free reference Q1 needed. In C and C++ it reaches 6.5–7× at 22 workers and is still rising, while `partition-lock` stops at 4.4–5.3×: there, the gap is contention and the cost of the shared index. In Java and Go `partition-lock` is close to the lock-free build (4.4 against 4.5, 3.1 against 3.9), so their ceiling is mostly the hardware and the runtime, not the locks.

#### Q2 follow-up: does the merge algorithm decide the answer? (Q2b, 2026-11-16)

The answer: **no. Sharing still wins with a better merge, so the Q2 result does not depend on the merge algorithm.**

**Why the follow-up.** `private-merge` merges each term's N lists with a linear scan: for every posting it compares the heads of all k lists (k ≤ N), so frequent terms, which appear in every worker's index, cost N comparisons per posting. With N merging threads the work per thread then stays constant, which would explain the flat merge time. `private-merge-heap` is identical except that it keeps the list heads in a binary min-heap, about log k comparisons per posting.

**Experiment.** The Q2 calibration had shown the merge is not small (0.13-0.31 s against a build of 0.2-0.65 s on 128 MB, where the plan predicted 10-25% of the total), and a linear scan is the textbook slow choice, so a reader may ask whether Q2's verdict (replicating against sharing) is a verdict on replicating or on this merge. `private-merge-heap` replaces only the merge by a binary heap (O(log k)) and keeps everything else. `sequential` and `private-merge-heap`, build mode, strong scaling, 128 MB, workers as in Q1, 1 warm-up + 5 repetitions (`bench/params-q2b.sh`, 36 runs, estimated at 6.5 minutes); its `finalize_ns` is compared with that of `private-merge` (Q2 or Q4b, the same kind of runs).

- *If `finalize_ns` barely changes:* the merge is not bound by the comparisons but by memory (reading k private lists, writing the output, allocation), so the algorithm does not decide the answer and Q2's conclusion stands.
- *If it falls a lot at high N:* the linear scan was the cost, and Q2 must be read as "replicating with a naive merge"; the heap result is then the fair one for the comparison with `partition-lock`.
- *Prediction:* a small difference up to 6 workers (k is small, and a heap costs more per comparison than a scan: swaps, unpredictable branches), a gain growing with N above 8 for the common terms (k close to N), and little change in total time, because most terms are rare (k = 1, no merge at all) and the build dominates. The heap may even lose at small N.

**Run.** `results/q2b/20261116-164124/`: 36 runs × (1 + 5), about 7 minutes, no failures; build, 128 MB, strong. Compared with `private-merge` and `partition-lock` (P64 B64) from the night run (`results/q2/`), and with the second measurement of `private-merge` in `results/q4b/`. Medians of the 5 repetitions.

| Merge time (`finalize_ns`), 22 workers, 128 MB | Java | Go | C++ | C |
| --- | --- | --- | --- | --- |
| linear scan, Q2 | 0.117 s | 0.170 s | 0.295 s | 0.294 s |
| linear scan, Q4b (same variant, another run) | 0.143 s | 0.140 s | 0.296 s | 0.299 s |
| heap, Q2b | 0.060 s | 0.139 s | 0.276 s | 0.271 s |
| change, heap against Q2 | −48% | −18% | −7% | −8% |

| Total (build + merge), 22 workers | Java | Go | C++ | C |
| --- | --- | --- | --- | --- |
| `private-merge-heap` / `partition-lock` | 0.43 / 0.39 s | 0.44 / 0.38 s | 0.54 / 0.32 s | 0.41 / 0.22 s |

- **Java:** the heap halves the merge, and the merge now shrinks as workers are added: 0.125 s at 2 workers, 0.060 s at 22 (the linear scan stayed at 0.11–0.13 s). The gain grows with the number of workers (heap against linear scan: 1.02 at 2 workers, 0.74 at 6, 0.52 at 22).
- **Go:** the merge also shrinks with workers (0.225 s at 2, 0.139 s at 22), but its gain at 22 workers is within the spread of the linear baseline itself: the same linear variant gave 0.170 s in Q2 and 0.140 s in Q4b, and the heap gave 0.139 s. It is not a measured gain.
- **C and C++:** the merge changes by 5–10% (0.29 → 0.27–0.28 s), within noise, and stays flat in the number of workers (C++ 0.25–0.28 s, C 0.25–0.31 s from 2 to 22 workers).
- **`partition-lock` keeps the lower total in every language** (table above). With the heap `private-merge` totals change by at most 15% (Java), 7% (Go) and 5% (C++, C) against the linear scan, nowhere near the 20–40% needed to close the gap.

**This confirms the Q2b prediction recorded before the run:** a small difference at few workers (at 2 workers the merge is 0.95–1.02 of the linear scan's in all four languages, and in Go, C++ and C it stays 0.93–1.05 up to 6 workers; the total is 4% slower in Java and C at 2 workers, within noise, so the heap does not win at small N), a gain that grows with N where the comparisons mattered (Java: the merge is already 0.80 and 0.74 of the linear scan's at 4 and 6 workers, a little earlier than predicted, with no effect on the total at 4–6 workers: 1.01), and little change in the total, because the build dominates and most terms are rare. The Q2 result therefore does not depend on the merge algorithm.

**What it shows.** In Java the linear scan did cost something at high N: the merge halves with the heap. In C and C++ it did not (5–10%), and in Go the effect is not measurable, so there the merge's cost is not the comparisons. The likely reason, a hypothesis consistent with these numbers and not measured (no memory bandwidth counters were read), is memory traffic: reading N lists scattered through RAM, writing the merged list, looking each term up in N maps, which more threads do not speed up because they share the same memory. It is the same suspect as Q1's ceiling for `partition-lock` and Q2's weak scaling.

### Q3. Locks or messages?

**The strategy in short.** In `partition-lock` every partition has a lock, and anyone may write into it, taking turns. In `partition-writers` every partition has one owner, the only thread that may write into it; the others send it their postings as messages. Q3 asks which way of protecting shared data costs less.

Picture the dictionary split into sections (A–C, D–F, …):

- **Locks:** whoever has a word for D–F waits until the D–F notebook is free, then writes it in.
- **Messages:** each section has a person in charge. Whoever has a word for D–F writes a note, drops it in that person's mailbox and goes straight back to work. From time to time the person in charge empties the mailbox and copies the notes into the notebook.

The two prices compared:

|  | Locks | Messages |
| --- | --- | --- |
| Waiting | in the queue for the notebook | none, until the mailbox is full |
| Cost per write | taking the lock | writing the note (a copy) and dropping it in the mailbox, which itself must be protected |
| Who does the writing | anyone | only the owner: every posting of "the" is written by one thread |
| Full mailbox | — | the sender stops (backpressure) |

How the experiment answers:

- **Same conditions.** `partition-writers` against `partition-lock` with the same P and batch, so the only difference is locks against messages.
- **Knob 1, the batch.** Batch 1 sends one note per posting; batch 1024 sends one note carrying 1024 postings. As in Q1, it sets how often the fixed cost is paid, here the cost of one mailbox operation.
- **Knob 2, the mailbox capacity (`QUEUE`).** A small mailbox stops senders often; a large one rarely, at the cost of memory.
- **Cross-checks.** Voluntary context switches count how often a thread waited (full or empty mailbox); CPU utilization shows how much of the time was useful work.

How the result is read:

| If we see… | It means… |
| --- | --- |
| messages lose at batch 1 but tie or win at large batches | each message is expensive, but amortized it works |
| messages win at every batch | removing lock contention matters more than the mailbox cost |
| messages always lose | copying and managing mailboxes costs more than the locks |
| a large mailbox clearly beats a small one | the bottleneck is backpressure: senders wait for the owner |

**The link with Q1.** Q1 showed that the real problem is serialization (contention), not the cost of taking a lock. Messages remove contention on the locks, but not the serialization: the owner of the partition holding "the" still writes all its postings alone. So the most interesting prediction is that messages do not break `partition-lock`'s ceiling: the bottleneck moves from the lock to the owner.

In one sentence: same configuration, locks against messages; the batch sets what each message costs, the mailbox how much senders wait, and the hot partition limits both.

**Fundamentals.**

- **Owner computes.** Instead of protecting shared data with locks, give each partition one owner thread; nobody else touches it. Other workers send it messages. No lock on the index is ever needed.
- **The queue is shared.** Sending a message goes through a queue, which itself needs a lock or atomic operations. Synchronization is moved, not removed, so batching matters as in Q1.
- **Copying and backpressure.** A message copies postings, and a full queue blocks the sender. Queue capacity trades memory for fewer stalls.
- **Each language has its own idiom.** Go is built around channels; Java has `ArrayBlockingQueue`; C++ and C have no standard queue, so ours is a ring buffer (`std::deque` in C++) behind a mutex. All four are used through non-blocking put and take; a sender facing a full queue empties its own queue and retries instead of sleeping (docs/design.md, section `partition-writers`).

**Why it matters.** It is the shared-memory versus message-passing debate of the course, measured on the same problem in the same languages.

**Experiment.** `partition-writers` against `partition-lock`, same P and `BATCH`, plus `QUEUE` capacity. Build mode, where writing dominates (the variant has no mixed modes: its owners write without locks). The parameter file is `bench/params-q3.sh`: `sequential`, `partition-lock` and `partition-writers`, P = 64, `BATCH` 1, 64, 1024, `QUEUE` 4 and 64, strong scaling on the 128 MB corpus, workers `1 2 4 6 8 12 16 22`, 1 warm-up + 5 repetitions (292 runs, estimated at 63 minutes: see design.md, Experiment Q3). Waiting is a polling loop with yields, so blocked senders show up as CPU time rather than voluntary context switches (design.md, section `partition-writers`); the cross-check of the plan reads CPU utilization and the throughput, not only `ctx_vol`.

**Prediction.**

- With batch 1, messages lose clearly to locks: one queue operation per posting is more expensive than one lock.
- With batch 1024, `partition-writers` matches or beats `partition-lock`: the owners never contend with each other.
- The hot-partition limit of Q1 still holds: the owner of the "the" partition is the bottleneck.
- Go is the language where messages come closest to locks.

**What would surprise us.** Go's channels being slower relative to its mutexes than Java's queues are to its locks.

#### Q3 results (sweep of 2026-11-16)

The answer to Q3: **messages and locks end up almost level.** One message per posting is a disaster; with batches of 64 and a large enough mailbox, messages beat locks by 3–14%, but they hit the same ceiling. Messages move the bottleneck, they do not remove it.

**Run.** `results/q3/`, 292 runs × (1 + 5), 61 minutes, no failures. Build mode, 128 MB, P = 64, batch 1 / 64 / 1024, queue 4 / 64. Ratio = time of `partition-lock` / time of `partition-writers` at the same batch (> 1 = messages faster).

|  | Java | Go | C++ | C |
| --- | --- | --- | --- | --- |
| batch 1, 22 workers (queue 4 / 64) | 0.04 / 0.08 | 0.18 / 0.33 | 0.09 / 0.09 | 0.07 / 0.08 |
| batch 64, queue 64, geometric mean over 2–22 workers | 1.06 | 1.06 | 1.07 | 1.06 |
| batch 1024, 22 workers (queue 4 / 64) | 0.96 / 0.96 | 0.87 / 0.95 | 0.92 / 0.95 | 0.81 / 0.80 |
| best `partition-lock` speedup | 4.31 | 3.43 | 5.63 | 4.57 |
| best `partition-writers` speedup (batch 64, queue 64) | 4.91 | 3.70 | 5.98 | 4.71 |

**1. Batch 1 loses clearly: confirmed.** From 2 workers up messages are 2–25× slower than locks, and slower than `sequential` at every worker count. At 1 worker the two tie, so the loss is the mailbox traffic, not the copying.

**2. Batch 1024 matches or beats locks: confirmed up to 8–12 workers, not at 16–22.** The best messages configuration is batch 64 with a mailbox of 64, not batch 1024; with a mailbox of only 4, batch 64 loses (0.78–1.01). The mailbox matters: at batch 64, the larger one is 6–40% faster.

**3. The hot partition still limits: confirmed as a ceiling.** The best messages speedups are only 3–14% above the best locks and flatten above 8–12 workers the same way. The data has no per-owner load, so "the owner of the hot partition is the bottleneck" is the likely cause, not a measured one.

**4. Go is where messages come closest to locks: confirmed at batch 1, no difference at larger batches.** At batch 1 Go's channels lose least (0.18–0.33 at 22 workers against 0.04–0.09 elsewhere); at batch 64 and 1024 all four languages are within noise of each other.

**How waiting shows up depends on the language.** Under the plan's deadlock rule, a sender facing a full mailbox empties its own and yields instead of sleeping. At batch 1, 22 workers:

- **Go:** waiting is CPU time in user space (utilization 94–132%, only 6% kernel time); the goroutine yields inside the runtime.
- **C and C++:** CPU time spent mostly in the kernel (80–83%, `sched_yield`), and also millions of voluntary context switches from each mailbox's contended mutex; CPU seconds are 27–42× those of the lock.
- **Java:** utilization collapses to 17–23%: `ArrayBlockingQueue` puts the threads to sleep.

So CPU utilization together with throughput is the indicator that works in all four languages; voluntary context switches alone do not.

### Q4. What does a hybrid CPU do to scaling?

**The strategy in short.** The Q1 curves bend: they grow well up to 4–6 workers, then slow down. Q4 asks whose fault each bend is: the software (locks, contention) or the hardware. The 155H does not have identical cores:

| Core | Count | Speed |
| --- | --- | --- |
| P-core | 6 (12 threads with hyperthreading) | fast |
| E-core | 8 | about half |
| LP E-core | 2 | slower still |

Picture a moving crew of 6 professionals, 8 apprentices and 2 interns. The seventh person is not one more professional but an apprentice. And if everyone must carry the same number of boxes, the move ends when the intern finishes.

The two suspects for every bend in a curve:

- **Software:** threads get in each other's way (locks, waiting). The bend depends on the variant.
- **Hardware:** the extra cores are slower or shared (hyperthreading). The bend sits at the worker count where a core type runs out: 6, 12, 14 or 16.

Three tools, each removing one suspect:

1. **The pinned control experiment.** The same runs, with threads forced onto the P-cores only (`taskset`, up to 12 workers). The E-cores leave the game: a bend present unpinned that disappears pinned belongs to the E-cores.
2. **A lock-free reference.** `private-merge` (Q2) has no contention while building, so its bends are hardware only. If `partition-lock` bends where `private-merge` bends, it is the hardware; if it bends earlier, it is the software.
3. **Static against dynamic.** `private-merge-static` splits the documents into equal parts up front; `private-merge` hands them out dynamically. It is the same variant except for how work is divided. With identical cores the two should match; if static loses, it is because the slow cores finish last and everyone waits for them.

How the result is read:

| If we see… | It means… |
| --- | --- |
| a bend at 6 workers, both unpinned and pinned | hyperthreading: from the 7th worker, two threads share a core |
| a bend at 12–16 workers only unpinned | the E-cores |
| `partition-lock` bends before `private-merge` | the software (contention) |
| static far worse than dynamic from 7 workers up, but equal pinned | core heterogeneity: equal shares of work penalize the fast cores |
| static equal to dynamic even unpinned | the operating system moves threads to the fast cores on its own |

**What Q1 already shows.** `global-lock` collapses at any worker count: that is software. `partition-lock` at P = 64 keeps rising slowly up to 16–22 workers, the typical shape of slower cores being added, but only the pinned control can say so with certainty.

In one sentence: three tools, each removing one suspect: pinning removes the E-cores, `private-merge` removes the locks, and static against dynamic shows what ignoring different core speeds costs.

**Fundamentals.**

- **Not all cores are equal.** The 155H has 6 P-cores (with hyperthreading: 12 threads), 8 E-cores and 2 LP E-cores. An E-core does roughly half the work of a P-core per second, an LP E-core less.
- **Hyperthreading shares a core.** Two threads on one P-core share its execution units; the second typically adds 20–30%, not 100%.
- **Makespan.** A parallel phase ends when its slowest worker ends. If work is split into N equal parts up front (static), the worker on the slowest core decides the time.
- **Dynamic handout** (a shared counter: whoever is free takes the next document) lets fast cores take more work, so all workers finish at about the same time.

**Why it matters.** Textbook speedup assumes identical cores. Every laptop and phone CPU today is hybrid, and most parallel code still splits work statically.

**Experiment.** The strong-scaling curves of every variant, unpinned and in the P-cores-only control. Plus `private-merge-static` (static split) against `private-merge` (dynamic), build mode, in one language. Two parameter files, because a params file has one `PINNING`: `bench/params-q4a.sh`, the pinned control (`taskset` to CPUs 0-11, workers `1 2 4 6 8 12`) for `sequential`, `partition-lock` (P = 64, batch 64), `private-merge` and `private-merge-static` (58 runs, about 10 minutes); and `bench/params-q4b.sh`, unpinned, workers `1 2 4 6 8 12 16 22`: `private-merge-static` in C against `private-merge` in all four languages, with `sequential` (40 + 4 runs, about 7 minutes). The unpinned `partition-lock` curve is the one of Q1. `private-merge` and `sequential` are in `q4b` so that the static-against-dynamic comparison did not have to wait for Q2; the numbers are the same kind of runs as Q2's strong scaling on 128 MB.

**Prediction.**

- Every curve bends at 6 workers, where the P-cores run out. From 7 to 12 the gain is 20–30% (hyperthreading); from 13 to 22 the E-cores add less, and 22 may be slower than 16.
- In the pinned control, the bends at 14 and 16 disappear (no E-cores); the bend at 6 stays, because it is hyperthreading.
- `private-merge-static` at 22 workers is no faster than `private-merge` at 8–10, because the LP E-core workers finish last. Dynamic handout removes the gap.

**What would surprise us.** No difference between static and dynamic would mean the Linux scheduler moves threads to fast cores well enough on its own.

#### Q4 results (sweep of 2026-11-16)

The answer to Q4: **the bend at 6 workers is the hardware**, the same in every variant and the same pinned or not; above 12 workers the extra cores add little. Splitting work statically costs about 18% of the build on this hybrid CPU, much less than predicted, and the merge hides even that in the total.

**Run.** `results/q4a/` (pinned to the P-cores, CPUs 0–11, up to 12 workers: 58 runs, 9.5 minutes) and `results/q4b/` (unpinned `private-merge-static` with its references: 44 runs, 7 minutes); the unpinned curves of `partition-lock` and `private-merge` come from Q2 (they agree with Q1 and Q4b within 4% at most points). Build mode, 128 MB.

| Gain per step, ideal in brackets | 4→6 (1.50) | 6→8 (1.33) | 8→12 (1.50) | 12→16 (1.33) | 16→22 (1.38) |
| --- | --- | --- | --- | --- | --- |
| `partition-lock`, unpinned | 1.26–1.30 | 1.08–1.16 | 1.16–1.20 | 1.07–1.10 | 0.98–1.04 |
| `partition-lock`, pinned | 1.24–1.29 | 1.08–1.13 | 1.11–1.17 | — | — |
| `private-merge`, unpinned | 1.19–1.29 | 1.04–1.12 | 1.05–1.21 | 0.95–1.03 | 1.03–1.10 |

**1. Every curve bends at 6; 7–12 workers add 20–30%; more cores add less: confirmed.** From 6 to 12 workers `partition-lock` gains 25–37% (hyperthreading, as predicted); from 12 to 22 only 6–14%. 22 workers are not slower than 16 beyond noise.

**2. Pinned, the bend at 6 stays: confirmed; the bends at 14 and 16: unclear.** Pinned and unpinned runs are the same up to 12 workers (time ratio 0.94–1.09 for `partition-lock`), with the same step gains. So the operating system already places the first 12 threads on the P-cores, and the bend at 6 is hyperthreading, not the E-cores. The pinned runs stop at 12 workers, so there is no pinned point to compare 14, 16 and 22 with.

**3. Static at 22 workers no faster than dynamic at 8–10: contradicted; a smaller static penalty exists.** On the total, static is between 3% faster and 10% slower than dynamic at every worker count, and static at 22 workers is 12% faster than dynamic at 8. The penalty is in the build alone: unpinned, static is 16–19% slower than dynamic from 8 workers up (0–5% below that); pinned, only 4–8%. So equal shares of work do cost something once slower cores are involved, but far less than predicted, and the merge (0.26–0.31 s, the same for both) hides it in the total. The likely reason it is small: the operating system's scheduler (with Intel's Thread Director on this CPU) moves the lagging threads to faster cores, which a static split cannot do for itself.

**Lock-free reference.** `private-merge`'s build keeps rising slowly after 12 workers, while `partition-lock`'s total flattens after 16. Both bend at 6, so that bend is the hardware; the flattening above 16 is the hardware plus, in C and C++, a part that the lock-free build escapes (see Q2, Closing Q1).

### Q5. Reads against writes: does a read/write lock pay off?

**The strategy in short.** With a plain lock, two queries reading the same partition wait for each other, although reading together breaks nothing: only a writer needs to be alone. A read/write lock lets all readers in together and keeps others out only while someone writes. Q5 asks whether that pays off, and when.

Picture a library with one reference book:

- **Plain lock:** one person at a time in the book's room, whether to read it or to correct a page.
- **Read/write lock:** many may read together. Whoever must correct a page waits for the room to empty, and nobody enters while they write.

The two prices compared:

|  | Plain lock | Read/write lock |
| --- | --- | --- |
| Two readers at once | wait for each other (needlessly) | read in parallel |
| Cost of each entry | low | higher: the lock counts its readers, and that counter is shared |
| A writer | waits its turn | waits for every reader to leave |
| Risk | — | starvation: with a steady stream of readers, a writer may wait long |

**The link with Q1.** Our queries look for frequent words, so they often land in the hot partition, the one holding "the". Q1 showed that the real problem is contention; the read/write lock attacks exactly that, but for readers only. And each query holds its lock long (about 50 µs, returning about 26,000 documents), so the extra entry cost is small by comparison.

How the experiment answers:

- **Same conditions.** `partition-rwlock` against `partition-lock`, same P and batch; the only difference is the kind of lock.
- **Knob 1, the reader/writer mix.** Query mode (100% reads), then mixed-95-5, 50-50 and 20-80: it shows up to which point the read/write lock pays.
- **Knob 2, the worker count.** More readers, more potential gain.
- **Two measures.** Throughput (operations per second, from `run_ns`) and query latency (p50 and p99, from the latency files). The p99 shows whether some queries are stuck for long behind a writer.
- **Two languages.** Java (`ReentrantReadWriteLock`) and C (`pthread_rwlock_t`) have different rules about who goes first when readers and writers wait together: glibc favours readers, Java tries not to keep writers waiting too long. Comparing them shows the effect of those rules.

How the result is read:

| If we see… | It means… |
| --- | --- |
| the read/write lock wins in query and 95-5, ties at 50-50, loses at 20-80 | it pays when readers dominate; with many writes the extra cost does not pay back |
| it loses even in query mode | the reader counter costs more than reading together gains |
| throughput improves but p99 gets worse | better on average, but some queries are stuck behind writers |
| C and Java behave differently at 95-5 | the precedence rules between readers and writers matter |

In one sentence: same configuration, plain lock against read/write lock; the reader/writer mix says how far it pays, the p99 latency says who pays for it.

**Fundamentals.**

- **Readers do not conflict.** Two queries reading the same list are safe together; only an insert needs exclusive access. A read/write lock lets any number of readers in at once.
- **But readers still write.** Taking a read lock updates a shared reader count, so the lock's cache line still moves between cores. A read/write lock is more expensive per acquisition than a plain lock.
- **Long critical sections favour it.** Our queries hit frequent words and return about 26,000 document IDs each, so a query holds its lock for about 50 µs. Against that, the acquisition cost is negligible.
- **Fairness.** A reader-preferring lock (glibc's default) can starve writers; a writer-preferring one makes queries wait behind inserts.

**Why it matters.** Real indexes are read-heavy. Whether readers can share is the first question a production design asks.

**Experiment.** `partition-rwlock` against `partition-lock`. Query mode and the three mixed modes, strong scaling, in Java and C. Read throughput and p50 / p99 query latency. The parameter file is `bench/params-q5.sh`: `sequential`, `partition-lock` and `partition-rwlock` in Java and C, P = 64, batch 64, on the 512 MB query corpus, workers `1 2 4 6 8 12 16 22`, 1 warm-up + 5 repetitions, 136 runs. **`OPS_TOTAL` is 40,000 instead of 100,000** to fit the time limit (estimate 64 minutes, against 95 with 100,000): every repetition rebuilds the 512 MB index (load and build are 40-70% of a repetition), so the cut only shortens the timed phase. What the question loses: p99 and throughput come from 40,000 operations (38,000 queries in mixed-95-5, 8,000 in mixed-20-80), and at 22 workers the timed phase of a read/write-locked query run is only about 0.4 s (C) to 0.7 s (Java); the 5 repetitions give the spread.

**Prediction.**

- In query mode and mixed-95-5 the read/write lock wins clearly, scaling close to the hardware limit.
- At 50-50 the two are close; at 20-80 the plain lock wins (more expensive acquisitions, few readers to share).
- p99 latency is worse with the read/write lock under heavy writes, because queries queue behind writers.
- C and Java differ at 95-5 because their locks have different fairness policies.

**What would surprise us.** The read/write lock losing in query mode would mean its shared counter costs more than the 50 µs queries it lets run in parallel.

#### Q5 results (sweep of 2026-11-16)

The answer to Q5: **the read/write lock pays off far more than predicted, and in every mix.** In query mode it gives 5.9× (Java) and 7.1× (C) the throughput of a plain lock at 22 workers, it still wins with 80% writes, and its tail latency is better everywhere. The plain lock stops scaling at 4–6 workers because readers queue for each other behind long queries.

**Run.** `results/q5/`, 136 runs × (1 + 5), 58 minutes, no failures. Java and C, P = 64, batch 64, 512 MB corpus, 40,000 operations per repetition (cut from 100,000 to fit the hour: the timed phase at 22 workers is 0.4–3.4 s, and in mixed-20-80 the p99 rests on 80 queries per repetition). Throughput = operations / `run_ns`; latencies from the latency files.

| 22 workers, rwlock / plain lock | Java throughput | Java p99 | C throughput | C p99 |
| --- | --- | --- | --- | --- |
| query | 5.90× | 0.11 | 7.12× | 0.08 |
| mixed-95-5 | 4.14× | 0.20 | 3.75× | 0.05 |
| mixed-50-50 | 1.55× | 0.51 | 1.97× | 0.12 |
| mixed-20-80 | 1.11× | 0.82 | 1.28× | 0.32 |

Throughput > 1 and p99 < 1 favour the read/write lock. In query mode the plain lock peaks at 1.3–1.5× `sequential` at 4–6 workers and then falls, while the read/write lock keeps rising to 7.3× (Java) and 8.1× (C) at 22 workers.

**1. Clear win in query and mixed-95-5: confirmed; "close to the hardware limit": unclear.** The win starts at 2 workers and grows with them. Mixed-95-5 flattens at 4–5× `sequential` from 8 workers: the 5% inserts, not the hardware, limit it. No hardware reference exists for this workload, so the second half cannot be decided.

**2. Close at 50-50, plain lock wins at 20-80: contradicted.** At 50-50 the read/write lock is ahead from 4 workers (1.25–1.97×); at 20-80 the plain lock never wins beyond noise, and the read/write lock is 8–28% ahead from 4–8 workers up. The prediction underestimated how unequal the two kinds of operation are: a query holds its lock for tens of microseconds (long lists of frequent words), an insert batch only briefly, so letting the readers share matters even when they are few.

**3. Worse p99 under heavy writes: contradicted.** From 4 workers up, the read/write lock's p99 is lower in every mode, with one tie: Java in `mixed-20-80` at 6 workers (0.98 of the plain lock's, within noise; 0.88 at 4 and 0.73 at 8) (at 22 workers: 0.05–0.20 of the plain lock's in 95-5, 0.32–0.82 in 20-80). The one place where typical queries wait longer is Java's median at 22 workers in the mixed modes (1.02–1.15× the plain lock's), while its tail is still shorter.

**4. C and Java differ at 95-5 because of fairness: the difference exists, the cause is unclear.** Throughput is almost the same (99.6k against 93.4k operations/s), but Java's p99 is 2.8–4× C's in the mixed modes and only 1.13× in query mode, where there are no writers. The gap opens exactly when writers are present, which is what different reader/writer precedence would produce; but Java and C also differ in runtime, garbage collection and allocation, and no run isolates the lock policy alone.

The surprise named above (the read/write lock losing in query mode) did not happen: at 1 worker it is 9% slower in Java and level in C, and from 2 workers up it wins.

### Across all questions: does the language change the answer?

**Fundamentals.** Java and Go have garbage collectors and runtime threads that run on other cores even when the program has one worker. Java also compiles at run time (JIT). C and C++ have none of that, and C uses our own hash map. The same strategy therefore costs different amounts in each language.

**Prediction.**

- The ranking of strategies is the same in all four languages: the principles above do not depend on the language.
- The absolute times do. The sizing pass already found that the language ranking is not obvious: C++ had the slowest sequential build at the largest size.
- Java and Go show lower relative speedup than C and C++, because at 1 worker their garbage collectors already use other cores. Their CPU time at 1 worker is above 1× the wall time; C's is about 1×.
- Java and Go use about twice C's memory, as the sizing pass measured, and the gap grows in `private-merge`.

## The implementations

Four core variants in all four languages (16 implementations) answer Q1–Q4 and the language question; three optional extras add Q5 and the clean static-split test of Q4.

| Variant | Partitioning | Writer | Coordination | Numeric parameters | Modes | Languages | Answers |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `sequential` (done) | single | direct | none | — | all five | all four | baseline for every question |
| `global-lock` | single | direct | one lock, whole index | `batch` | all five | all four | Q1, Q4 |
| `partition-lock` | by term | direct | one lock per partition | `partitions`, `batch` | all five | all four | Q1, Q3, Q4 |
| `private-merge` | by document | direct, private | none while building; parallel merge at the end | — | build, query | all four | Q2, Q4 |
| `partition-writers` | by term | owner per partition (the workers themselves) | bounded queues | `partitions`, `batch`, `queue` | build, query | all four | Q3, Q4 |
| `private-merge-heap` (extra) | by document | direct, private | none while building; parallel merge at the end, with a heap | — | build, query | all four | Q2 follow-up (Q2b) |
| `private-merge-static` (extra) | by document | direct, private | none; static split of documents | — | build | C only | Q4 |
| `partition-rwlock` (extra) | by term | direct | read/write lock per partition | `partitions`, `batch` | query, mixed (`build` is declared too, because `bench/check.sh` builds its mixed-mode reference from it; Q5 does not run it) | Java, C | Q5 |

### Rules every variant follows

- **Dynamic handout.** Workers take documents from a shared counter; only `private-merge-static` splits them up front, to show what that costs.
- **Batch buffers.** Each worker keeps one private buffer of postings per lock: one in `global-lock`, one per partition in `partition-lock`. A buffer is flushed, under its lock, when it holds exactly `BATCH` postings, whichever documents they come from, and every buffer is flushed at the end of each phase. Without buffers that cross documents, `BATCH` 1024 could never fill and `partition-lock` would take about P locks per document whatever `BATCH` is.
- **Posting lists always sorted.** Each new document ID is inserted in its correct place, almost always at or next to the end, so queries can merge lists directly.
- **Partial visibility in mixed modes.** A query reads lists as they are and may see a half-inserted document. It is the simplest to implement and adds no cost to compare.
- **Insert order of partitions.** In `partition-lock`, worker `w` visits partitions starting from `w mod P` and wrapping around (when it fills buffers and when it flushes), taking one lock at a time, so that workers do not all queue at partition 0. Queries use the fixed lock order below.
- **Fixed lock order.** An AND or OR query must read both lists at the same time, so it holds both partition locks, always taking the lower partition number first (once if both terms share a partition). Because every thread takes locks in the same order, two threads can never each hold the lock the other needs, so a deadlock is impossible.
- **Partition hash.** FNV-1a 64-bit of the term's UTF-8 bytes; the partition is (hash >> 32) mod P, the same in every language, so each partition holds the same terms everywhere. The high bits are used because C's hash map picks its slot from the low bits of the same hash; using those too would crowd each partition's terms into a fraction of C's slots.
- **Locks.** Each language's plain default, non-fair mutex: Java `ReentrantLock`, Go `sync.Mutex`, C++ `std::mutex`, C `pthread_mutex_t`. Non-fair means a running thread may take a free lock before a waiting one: faster, with a small risk that one thread waits long.
- **`sequential` stays the best sequential program.** It keeps its counting shortcut, so absolute speedup is measured against the fastest single-threaded version. A variant's overhead at 1 worker therefore includes its per-document counting, not only synchronization.

### How each variant works

- **`global-lock`.** Workers tokenize their documents in parallel and collect the postings in one private buffer. When the buffer holds `batch` postings, the worker takes the one index lock and inserts them all; batches cross document boundaries, and every buffer is flushed at the end of the phase. Queries take the same lock. The simplest correct design, and the floor every other variant must beat.
- **`partition-lock`.** The index is P sub-maps. A worker keeps one private buffer per partition and flushes a buffer under that partition's lock when it holds `batch` postings; batches cross documents, so `batch` really sets how often a lock is taken, and everything is flushed at the end. When a worker flushes several buffers at once, it visits partitions starting from (worker number mod P) and wraps around, so workers do not queue up at partition 0 one behind the other. Queries lock the partitions of their terms in fixed order (rules above).
- **`private-merge`.** Everyone builds their own index, then the indexes are combined. Each worker builds a private index from the documents it takes, with no locks, already divided into N piles by term (the partition hash with P = N). At the end, worker i merges pile i of every worker, so all workers merge at once, each on different terms, with no locks. Each worker's lists are already sorted (it takes increasing IDs), so merging them is an N-way merge in document order. The result is an index partitioned by term, which queries use directly.
- **`partition-writers`.** Each partition has exactly one owner, the only thread that writes it; everyone else sends it postings as messages. The owners are the workers themselves, so the variant uses exactly N threads like every other: each worker tokenizes documents and also owns 1/N of the partitions. Postings for another worker's partition go into that worker's bounded queue (`queue` capacity, in batches of `batch`); a worker regularly empties its own queue into its partitions. Deadlock rule: before waiting on another worker's full queue, a worker empties its own, so someone always makes progress. Build and query modes only: owners write without locks, so a query could not safely read during writes; in query mode nobody writes any more.
- **`private-merge-heap`.** `private-merge` with one change, the per-term N-way merge: instead of scanning the heads of the k lists for every posting (k comparisons), it keeps the heads in a binary min-heap keyed by document ID (log k comparisons). A term with one list is still reused as it is. Same build, same piles, same result.
- **`private-merge-static`.** `private-merge` with documents split into N equal ranges up front. In C only, because C has no garbage collector or JIT adding runtime threads that would blur the scheduling effect.
- **`partition-rwlock`.** `partition-lock` with a read/write lock per partition: queries share, inserts are exclusive. In Java (`ReentrantReadWriteLock`) and C (`pthread_rwlock_t`), whose locks have different fairness policies.

### What was cut, and why

| Cut | Reason |
| --- | --- |
| `central-writer` | Predictable ceiling (about 2×); `partition-writers` answers locks against messages better |
| Locks per posting list | Needs a concurrent map for adding new terms, which is a second synchronization problem; `partition-lock` covers fine-grained locking |
| Lock-free (CAS) | Hardest to get right and explain; a subtle bug gives fast, wrong numbers |
| Every variant in every language for the extras | 28 implementations was too many for one person; the extras need only the languages where the effect shows best |

### Order of work

Three rounds, each ending with `check.sh` passing and a sweep, so real speedup curves exist after round 1.

1. **Round 1:** `global-lock` and `partition-lock`, Java first, then Go, C++, C. Answers Q1 and the first half of Q4.
2. **Round 2:** `private-merge` and `partition-writers`, same order. Answers Q2, Q3 and the rest of Q4.
3. **Round 3 (if time allows):** `private-merge-static` (C), `partition-rwlock` (Java, C) and `private-merge-heap` (all four, Java first). Answers Q5, sharpens Q4 and gives the Q2 follow-up.
