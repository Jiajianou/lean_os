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
caveat that belongs on the record - and it is also why untracking them
costs little in this particular case: they crashed the *harness*, not the
kernel. `tests/fuzz/fuzz_leanfs.c` was calling `fake_pmm_reset()` per
input, which freed the 16 MiB inode table `leanfs_init` had begun caching
- a use-after-free in the test scaffolding. They are kept because they are
still perfectly good corpus inputs and because a reproducer that turned
out to be the harness's fault is exactly the sort of thing that should be
written down rather than quietly deleted.

## What is in git and what is not

**Nothing in this directory is tracked.** The rule is the simple one:
nothing a test *produces* is in git, only what a test *is*. `tests/*.c`,
`tests/sh/`, `tests/fakes/`, `tests/budgets.tsv` and
`tests/coverage-floor.tsv` are sources and stay tracked; everything under
`tests/corpus/` is output and does not.

**This reverses what this file used to say, and the reversal has a cost
that is worth stating rather than quietly dropping.** Reproducers -
`crash-*`, `leak-*`, `timeout-*`, `oom-*` - used to be un-ignored, on the
argument that an ignored file is one nobody is prompted to commit: it
sits in a working tree until the tree is thrown away, and the bug it
reproduces gets found again the hard way. That argument is still true.
What has changed is the decision, not the reasoning.

What is unchanged is that `make fuzz-run` replays every file in this
directory before it generates anything, so a reproducer still works as a
regression test **on the machine that found it** - which is the same
machine every tier of this project is run on. What is lost is a finding surviving a fresh clone. If
that matters for a particular input, the way to keep it is to turn it
into a test: a byte array in `tests/test_leanfs_format.c` is tracked,
readable, and says what it is for, which a hash-named blob never did.

If a committed corpus ever becomes worth it - to seed a longer campaign,
say - the way
to do it is a *minimised* one (`-merge=1` into a fresh directory) added
on purpose, not the accumulated output of whoever ran the fuzzer last.
