# Concurrent Inverted Index — Experiment Plan

Nov 15, 2026 · @Pietro Bertozzi

## Purpose

This plan fixes which questions the benchmark answers and which concurrent variants are built to answer them: five questions, four core variants in all four languages (16 implementations), plus up to three optional extras.

It complements `docs/design.md`, which fixes the shared infrastructure (workload, data, CLI contract, results, methodology). Once agreed, the questions and the variant list move into `design.md` as a new section, and this plan becomes the report's backbone: each question below is meant to be one chapter of the results.

**Time limit.** Every experiment (one params file per question) must take about one hour, builds included, and at most 70 minutes estimated (docs/design.md, Run conditions). The experiments below are therefore small, targeted sweeps, not one big sweep; `bench/params-q1.sh` is the first.

Status: draft, to iterate. Nothing here is implemented yet; the four `sequential` baselines are done and sized.

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

**Experiment.** `private-merge` against the best `partition-lock` from Q1. Build mode, strong and weak scaling. Read `build_ns`, `finalize_ns` (the merge), `peak_rss_bytes`.

**Prediction.**

- `private-merge` has the best build time at every N above 1.
- The merge takes 10–25% of total time at 16 workers if it is parallel; with a single merging thread it caps speedup at 4–5×.
- Peak memory grows clearly with N, by more than the other variants.
- In weak scaling, time grows slowly even with no locks: memory bandwidth, not synchronization, is the limit.

**What would surprise us.** `partition-lock` beating `private-merge` would mean the merge is more expensive than all the locking it replaces.

### Q3. Locks or messages?

**Fundamentals.**

- **Owner computes.** Instead of protecting shared data with locks, give each partition one owner thread; nobody else touches it. Other workers send it messages. No lock on the index is ever needed.
- **The queue is shared.** Sending a message goes through a queue, which itself needs a lock or atomic operations. Synchronization is moved, not removed, so batching matters as in Q1.
- **Copying and backpressure.** A message copies postings, and a full queue blocks the sender. Queue capacity trades memory for fewer stalls.
- **Each language has its own idiom.** Go is built around channels; Java has `BlockingQueue`; C++ and C have no standard queue, so ours is a mutex and a condition variable.

**Why it matters.** It is the shared-memory versus message-passing debate of the course, measured on the same problem in the same languages.

**Experiment.** `partition-writers` against `partition-lock`, same P and `BATCH`, plus `QUEUE` capacity. Build mode, where writing dominates (the variant has no mixed modes: its owners write without locks).

**Prediction.**

- With batch 1, messages lose clearly to locks: one queue operation per posting is more expensive than one lock.
- With batch 1024, `partition-writers` matches or beats `partition-lock`: the owners never contend with each other.
- The hot-partition limit of Q1 still holds: the owner of the "the" partition is the bottleneck.
- Go is the language where messages come closest to locks.

**What would surprise us.** Go's channels being slower relative to its mutexes than Java's queues are to its locks.

### Q4. What does a hybrid CPU do to scaling?

**Fundamentals.**

- **Not all cores are equal.** The 155H has 6 P-cores (with hyperthreading: 12 threads), 8 E-cores and 2 LP E-cores. An E-core does roughly half the work of a P-core per second, an LP E-core less.
- **Hyperthreading shares a core.** Two threads on one P-core share its execution units; the second typically adds 20–30%, not 100%.
- **Makespan.** A parallel phase ends when its slowest worker ends. If work is split into N equal parts up front (static), the worker on the slowest core decides the time.
- **Dynamic handout** (a shared counter: whoever is free takes the next document) lets fast cores take more work, so all workers finish at about the same time.

**Why it matters.** Textbook speedup assumes identical cores. Every laptop and phone CPU today is hybrid, and most parallel code still splits work statically.

**Experiment.** The strong-scaling curves of every variant, unpinned and in the P-cores-only control. Plus `private-merge-static` (static split) against `private-merge` (dynamic), build mode, in one language.

**Prediction.**

- Every curve bends at 6 workers, where the P-cores run out. From 7 to 12 the gain is 20–30% (hyperthreading); from 13 to 22 the E-cores add less, and 22 may be slower than 16.
- In the pinned control, the bends at 14 and 16 disappear (no E-cores); the bend at 6 stays, because it is hyperthreading.
- `private-merge-static` at 22 workers is no faster than `private-merge` at 8–10, because the LP E-core workers finish last. Dynamic handout removes the gap.

**What would surprise us.** No difference between static and dynamic would mean the Linux scheduler moves threads to fast cores well enough on its own.

### Q5. Reads against writes: does a read/write lock pay off?

**Fundamentals.**

- **Readers do not conflict.** Two queries reading the same list are safe together; only an insert needs exclusive access. A read/write lock lets any number of readers in at once.
- **But readers still write.** Taking a read lock updates a shared reader count, so the lock's cache line still moves between cores. A read/write lock is more expensive per acquisition than a plain lock.
- **Long critical sections favour it.** Our queries hit frequent words and return about 26,000 document IDs each, so a query holds its lock for about 50 µs. Against that, the acquisition cost is negligible.
- **Fairness.** A reader-preferring lock (glibc's default) can starve writers; a writer-preferring one makes queries wait behind inserts.

**Why it matters.** Real indexes are read-heavy. Whether readers can share is the first question a production design asks.

**Experiment.** `partition-rwlock` against `partition-lock`. Query mode and the three mixed modes, strong scaling, in Java and C. Read throughput and p50 / p99 query latency.

**Prediction.**

- In query mode and mixed-95-5 the read/write lock wins clearly, scaling close to the hardware limit.
- At 50-50 the two are close; at 20-80 the plain lock wins (more expensive acquisitions, few readers to share).
- p99 latency is worse with the read/write lock under heavy writes, because queries queue behind writers.
- C and Java differ at 95-5 because their locks have different fairness policies.

**What would surprise us.** The read/write lock losing in query mode would mean its shared counter costs more than the 50 µs queries it lets run in parallel.

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
| `private-merge-static` (extra) | by document | direct, private | none; static split of documents | — | build | C only | Q4 |
| `partition-rwlock` (extra) | by term | direct | read/write lock per partition | `partitions`, `batch` | query, mixed | Java, C | Q5 |

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
3. **Round 3 (if time allows):** `private-merge-static` (C) and `partition-rwlock` (Java, C). Answers Q5 and sharpens Q4.
