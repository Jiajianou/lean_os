# tests/corpus/

Inputs worth keeping, one directory per fuzz target.

Two kinds of thing live here and they are worth telling apart:

- **Corpus.** Inputs libFuzzer found interesting - each one reaches code
  no earlier input reached. They make the next campaign start where the
  last one finished instead of from nothing, which is most of what makes
  a nightly fuzz run better than a first one.
- **Reproducers.** An input that once crashed something. Checking one in
  is what turns a fuzz finding into a permanent regression test rather
  than an afternoon; `make fuzz-run` replays every file here before it
  starts generating anything, so a reintroduced bug fails immediately.

The two files in `leanfs/` beginning `crash-` are the second kind, with a
caveat that belongs on the record: they crashed the *harness*, not the
kernel. `tests/fuzz/fuzz_leanfs.c` was calling `fake_pmm_reset()` per
input, which freed the 16 MiB inode table `leanfs_init` had begun caching
- a use-after-free in the test scaffolding. They are kept because they are
still perfectly good corpus inputs and because a reproducer that turned
out to be the harness's fault is exactly the sort of thing that should be
written down rather than quietly deleted.

## What is in git and what is not

The split is in `.gitignore` and it is deliberate:

- **The corpus is ignored.** Every run adds files, so committing them
  would mean `make fuzz-run` dirties the tree with dozens of new entries
  each time - exactly the noise that makes people stop reading
  `git status`. The cost is that a campaign starts cold, which is a few
  seconds of a thirty-minute nightly run.
- **Reproducers are not.** `crash-*`, `leak-*`, `timeout-*` and `oom-*`
  are checked in. `make fuzz-run` passes `-artifact_prefix` so libFuzzer
  writes them straight into this directory rather than into the repo
  root, which means a fuzzer that finds something produces exactly one
  new file in `git status`, in the right place, and it is the one worth
  keeping.

That last part is the whole point of not ignoring them: an ignored file
is one nobody is prompted to commit. It sits in a working tree until the
tree is thrown away, and the bug it reproduces gets found again the hard
way.

If a committed corpus ever becomes worth it - to seed a longer campaign,
say - the way
to do it is a *minimised* one (`-merge=1` into a fresh directory) added
on purpose, not the accumulated output of whoever ran the fuzzer last.
