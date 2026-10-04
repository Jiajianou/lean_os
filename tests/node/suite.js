'use strict';

// Node's own regression tests, run on this machine the way tools/test.py runs
// them: each file in its own node process, with the flags its "// Flags:"
// line names, judged by its exit status. A test that calls common.skip()
// prints "1..0 # Skipped" and exits 0, and is counted as a skip rather than
// a pass. Arguments: the test directory, the suites to run (comma separated),
// an optional substring filter, and the per-test timeout in seconds.

const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const root = process.argv[2] || '/lib/node/test';
const suites = (process.argv[3] || 'parallel').split(',');
const filter = process.argv[4] && process.argv[4] !== '-' ? process.argv[4] : '';
const timeoutSeconds = Number(process.argv[5] || 120);

function flagsOf(file) {
  const head = fs.readFileSync(file, 'utf8').slice(0, 4096);
  const match = head.match(/^\/\/ Flags: (.*)$/m);
  return match ? match[1].trim().split(/\s+/) : [];
}

const counts = { pass: 0, fail: 0, skip: 0, timeout: 0 };
const failures = [];
const started = Date.now();

for (const suite of suites) {
  const directory = path.join(root, suite);
  const files = fs.readdirSync(directory)
    .filter((f) => /^test-.*\.(js|mjs|cjs)$/.test(f))
    .filter((f) => !filter || f.includes(filter))
    .sort();
  console.log(`[suite] ${suite}: ${files.length} tests`);
  for (const file of files) {
    const full = path.join(directory, file);
    const t0 = Date.now();
    const result = spawnSync(process.execPath, [...flagsOf(full), full], {
      cwd: root,
      timeout: timeoutSeconds * 1000,
      encoding: 'utf8',
      maxBuffer: 16 * 1024 * 1024,
      env: { ...process.env, NODE_TEST_DIR: root },
      detached: true,
    });
    // Whatever the test started goes with it, as tools/test.py does: a
    // cluster worker spinning in a loop outlived its killed parent once and
    // starved every test after it.
    if (result.pid) {
      try { process.kill(-result.pid, 'SIGKILL'); } catch {}
    }
    // And a scratch directory it could not clean up is moved out of the next
    // test's way rather than failing it: one test leaving an undeletable
    // entry in .tmp.0 failed the three hundred after it in tmpdir.refresh().
    // The test that left it has already failed on its own account.
    try {
      fs.rmSync(path.join(root, '.tmp.0'), { recursive: true, force: true });
    } catch {
      try { fs.renameSync(path.join(root, '.tmp.0'), path.join(root, `.tmp.stuck.${counts.fail}.${Date.now()}`)); } catch {}
    }
    const ms = Date.now() - t0;
    const out = (result.stdout || '') + (result.stderr || '');
    let verdict;
    if (result.error && result.error.code === 'ETIMEDOUT') {
      verdict = 'timeout';
    } else if (result.status === 0 && /^1\.\.0 # Skipped/m.test(result.stdout || '')) {
      verdict = 'skip';
    } else if (result.status === 0) {
      verdict = 'pass';
    } else {
      verdict = 'fail';
    }
    counts[verdict]++;
    const name = `${suite}/${file}`;
    if (verdict === 'fail' || verdict === 'timeout') {
      const why = result.signal ? `signal ${result.signal}` :
        result.error && result.status === null ? `spawn ${result.error.code}` : `exit ${result.status}`;
      const tail = out.trim().split('\n').slice(-14).map((l) => '    | ' + l.slice(0, 200));
      console.log(`[suite] ${verdict.toUpperCase()} ${name} (${why}, ${ms} ms)`);
      console.log(tail.join('\n'));
      failures.push(name);
    } else {
      console.log(`[suite] ${verdict} ${name} (${ms} ms)`);
    }
  }
}

const total = counts.pass + counts.fail + counts.skip + counts.timeout;
console.log(`[suite] ${total} tests in ${Math.round((Date.now() - started) / 1000)} s: ` +
            `${counts.pass} passed, ${counts.fail} failed, ${counts.timeout} timed out, ` +
            `${counts.skip} skipped`);
process.exitCode = 0;
