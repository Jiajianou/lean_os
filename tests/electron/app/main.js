'use strict';

// M227: Electron on this machine. Run by the [m227] self-test as
//
//   /usr/lib/electron/electron /lib/electron-test/app
//
// through Electron's own default_app, which is how `electron .` runs an app
// that is not packaged. Every check here is something the program works out
// for itself - a pixel value, a process id, a length in CSS pixels - because
// a runtime that starts and returns plausible values passes every test that
// only asks whether the calls returned.

const { app, BrowserWindow, ipcMain } = require('electron');
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const checks = [];
function check(name, fn) {
  checks.push({ name, fn });
}

let win = null;

check('this is Electron 46 on the browser\'s own Chromium, with Node 24.21.0 inside', () => {
  assert.strictEqual(process.versions.electron, '46.0.0-nightly.20260924');
  assert.strictEqual(process.versions.chrome, '155.0.8057.0');
  assert.strictEqual(process.versions.node, '24.21.0');
  assert.ok(process.versions.v8.endsWith('-electron.0'), process.versions.v8);
  assert.strictEqual(process.platform, 'linux');
});

check('Node in the main process: a file written comes back the same bytes', () => {
  const file = path.join(os.tmpdir(), 'electron-smoke.txt');
  const text = 'written by Electron on lean_os\n'.repeat(50);
  fs.writeFileSync(file, text);
  assert.strictEqual(fs.readFileSync(file, 'utf8'), text);
  fs.unlinkSync(file);
});

check('a window loads a page through a preload, and the page finishes loading', async () => {
  win = new BrowserWindow({
    width: 400,
    height: 300,
    useContentSize: true,
    show: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      sandbox: true,
      contextIsolation: true,
      nodeIntegration: false,
    },
  });
  await win.loadFile(path.join(__dirname, 'index.html'));
  assert.strictEqual(win.webContents.getTitle(), 'index.html');
});

check('the renderer is a process of its own', () => {
  const pid = win.webContents.getOSProcessId();
  assert.ok(pid > 0, 'renderer pid ' + pid);
  assert.notStrictEqual(pid, process.pid);
});

check('the page asks the main process across the context bridge and gets its answer', async () => {
  let text = 'waiting';
  for (let i = 0; i < 100 && text === 'waiting'; i++) {
    text = await win.webContents.executeJavaScript(
      'document.getElementById("answer").textContent');
    if (text === 'waiting') {
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  }
  const answer = JSON.parse(text);
  assert.strictEqual(answer.doubled, 42);
  // The reply carries the answerer's process id, because every other part of
  // this check would pass as well if the work had happened in the renderer.
  assert.strictEqual(answer.mainPid, process.pid);
  // And the page holds no Node: sandboxed, isolated, no integration.
  assert.strictEqual(answer.nodeInPage, 'undefined');
});

check('Blink laid the box out where the stylesheet put it', async () => {
  const rect = await win.webContents.executeJavaScript(
    'JSON.stringify(document.getElementById("box").getBoundingClientRect())');
  const r = JSON.parse(rect);
  assert.deepStrictEqual([r.x, r.y, r.width, r.height], [40, 30, 123, 45]);
});

check('capturePage: the body and the box are the colours they were given, edges exact', async () => {
  let image = null;
  // The first frame can arrive after the load event; ask until it has the box.
  for (let i = 0; i < 50; i++) {
    image = await win.webContents.capturePage({ x: 0, y: 0, width: 200, height: 100 });
    const size = image.getSize();
    if (size.width === 200 && size.height === 100) {
      const px = image.toBitmap();
      if (px[(50 * 200 + 100) * 4 + 2] === 200) {
        break;
      }
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  const size = image.getSize();
  assert.deepStrictEqual([size.width, size.height], [200, 100]);
  const px = image.toBitmap();
  // toBitmap is BGRA.
  const at = (x, y) => {
    const o = (y * size.width + x) * 4;
    return [px[o + 2], px[o + 1], px[o]];
  };
  const green = [0, 160, 0];
  const red = [200, 0, 0];
  assert.deepStrictEqual(at(5, 5), green, 'body');
  assert.deepStrictEqual(at(100, 50), red, 'box');
  assert.deepStrictEqual(at(39, 30), green, 'left of the box');
  assert.deepStrictEqual(at(40, 30), red, 'the box\'s top-left pixel');
  assert.deepStrictEqual(at(162, 74), red, 'the box\'s bottom-right pixel');
  assert.deepStrictEqual(at(163, 74), green, 'right of the box');
  assert.deepStrictEqual(at(40, 75), green, 'below the box');
});

ipcMain.handle('lean:double', (_event, n) => ({ doubled: n * 2, pid: process.pid }));

const deadline = setTimeout(() => {
  console.log('[m227] FAIL - the checks did not finish within 240 s');
  app.exit(2);
}, 240000);

app.whenReady().then(async () => {
  let failed = 0;
  for (const { name, fn } of checks) {
    try {
      await fn();
      console.log('[m227] ok   - ' + name);
    } catch (e) {
      failed++;
      console.log('[m227] FAIL - ' + name + ': ' + (e && e.stack || e));
    }
  }
  console.log('[m227] Electron ran ' + checks.length + ' checks, ' + failed + ' failed');
  clearTimeout(deadline);
  app.exit(failed ? 1 : 0);
});
