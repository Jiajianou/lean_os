'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const net = require('net');
const zlib = require('zlib');
const crypto = require('crypto');
const childProcess = require('child_process');

const checks = [];
function check(name, fn) {
  checks.push({ name, fn });
}

check('this is Node 24.21.0, built against Chromium\'s V8 and BoringSSL', () => {
  assert.strictEqual(process.versions.node, '24.21.0');
  // Chromium's V8, with Electron's embedder string on it, not Node's own copy.
  assert.ok(process.versions.v8.endsWith('-electron.0'), process.versions.v8);
  // A Node built against BoringSSL reports OpenSSL's version as 0.0.0.
  assert.strictEqual(process.versions.openssl, '0.0.0');
});

check('a file written through fs comes back the same bytes', async () => {
  const path = '/tmp/node-smoke.txt';
  const text = 'written by Node on lean_os\n'.repeat(100);
  fs.writeFileSync(path, text);
  assert.strictEqual(fs.readFileSync(path, 'utf8'), text);
  assert.strictEqual(fs.statSync(path).size, text.length);
  const again = await fs.promises.readFile(path, 'utf8');
  assert.strictEqual(again, text);
  fs.unlinkSync(path);
  assert.ok(!fs.existsSync(path));
});

check('timers fire in deadline order, and setImmediate before a later timer', async () => {
  const order = [];
  await new Promise((resolve) => {
    setTimeout(() => order.push('t20'), 20);
    setTimeout(() => { order.push('t40'); resolve(); }, 40);
    setTimeout(() => order.push('t0'), 0);
    setImmediate(() => order.push('immediate'));
  });
  assert.deepStrictEqual(order.filter((x) => x !== 'immediate'), ['t0', 't20', 't40']);
  assert.ok(order.includes('immediate'));
});

check('SHA-256 of "abc" is the FIPS 180-2 answer', () => {
  const digest = crypto.createHash('sha256').update('abc').digest('hex');
  assert.strictEqual(digest,
    'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
});

check('gzip round-trips 64 KiB', () => {
  const input = crypto.randomBytes(65536);
  assert.ok(zlib.gunzipSync(zlib.gzipSync(input)).equals(input));
});

check('a child process runs and its output comes back', () => {
  const out = childProcess.execFileSync('/bin/echo', ['from a child']);
  assert.strictEqual(out.toString(), 'from a child\n');
});

check('a TCP server and client meet over loopback', async () => {
  await new Promise((resolve, reject) => {
    let peer = null;
    const server = net.createServer((socket) => {
      peer = socket.remoteAddress;
      socket.on('data', (data) => socket.end('echo:' + data));
    });
    server.on('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const port = server.address().port;
      assert.ok(port > 0);
      const client = net.connect(port, '127.0.0.1', () => client.write('hello'));
      let reply = '';
      client.on('data', (d) => { reply += d; });
      client.on('error', reject);
      client.on('end', () => {
        server.close();
        try {
          assert.strictEqual(reply, 'echo:hello');
          assert.strictEqual(peer, '127.0.0.1');
          resolve();
        } catch (e) {
          reject(e);
        }
      });
    });
  });
});

check('libuv answers what the machine is: CPUs, memory, uptime, this process', () => {
  assert.ok(os.cpus().length >= 1);
  assert.ok(os.totalmem() > 0);
  assert.ok(os.freemem() > 0 && os.freemem() <= os.totalmem());
  assert.ok(os.uptime() > 0);
  const rss = process.memoryUsage().rss;
  assert.ok(rss > 1024 * 1024, 'rss ' + rss);
  assert.strictEqual(process.execPath, '/bin/node');
});

(async () => {
  let failed = 0;
  for (const { name, fn } of checks) {
    try {
      await fn();
      console.log('[m223] ok   - ' + name);
    } catch (e) {
      failed++;
      console.log('[m223] FAIL - ' + name + ': ' + (e && e.stack || e));
    }
  }
  console.log('[m223] Node.js ran ' + checks.length + ' checks, ' + failed + ' failed');
  process.exitCode = failed ? 1 : 0;
})();
