/*
 * Tests for the desktop app. They start the real Electron program with a
 * throwaway profile folder and the same pretend capture cards as the web
 * tests (fake-devices.js), so the real webcam is never switched on.
 *
 * Needs `npm install` to have been run in ../desktop as well as here.
 */
'use strict';

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { _electron: electron } = require('playwright-core');

const DESKTOP_DIR = path.resolve(__dirname, '..', 'desktop');
const ELECTRON_MODULE = path.join(DESKTOP_DIR, 'node_modules', 'electron');
const FAKE = path.join(__dirname, 'fake-devices.js');
const skip = fs.existsSync(ELECTRON_MODULE) ? false : 'run "npm install" in the desktop folder first';
const WAIT = { timeout: 15000 };

const WEBCAM = { kind: 'videoinput', deviceId: 'cam1', label: 'HP True Vision HD Camera', groupId: 'g-cam' };
const MIC = { kind: 'audioinput', deviceId: 'mic1', label: 'Microphone Array', groupId: 'g-mic' };
const CARD = { kind: 'videoinput', deviceId: 'card1', label: 'USB3.0 HD Video Capture', groupId: 'g-card' };
const CARD_SOUND = { kind: 'audioinput', deviceId: 'card1a', label: 'Digital Audio Interface (USB Digital Audio)', groupId: 'g-card' };

let profile;

before(() => { profile = fs.mkdtempSync(path.join(os.tmpdir(), 'console-viewer-test-')); });
after(() => { try { fs.rmSync(profile, { recursive: true, force: true }); } catch { /* still locked */ } });

function testEnv(userData, fakeMedia = true) {
  const env = { ...process.env, CV_USER_DATA: userData };
  // Until our pretend devices are loaded, make the browser engine use its
  // own dummy camera so the real one stays off.
  if (fakeMedia) env.CV_FAKE_MEDIA = '1';
  // Terminals inside VS Code set this, and it makes Electron behave as plain
  // Node instead of opening a window.
  delete env.ELECTRON_RUN_AS_NODE;
  return env;
}

// Starts the program. With `devices`, the page is reloaded with those
// pretend devices in place of real hardware.
async function launch(t, options = {}) {
  const {
    args = [],
    devices = [CARD, CARD_SOUND],
    executablePath = require(ELECTRON_MODULE),
    appArgs = [DESKTOP_DIR],
    userData = fs.mkdtempSync(path.join(profile, 'p-')),
    fakeMedia = true,
  } = options;
  const app = await electron.launch({
    executablePath,
    args: [...appArgs, ...args],
    env: testEnv(userData, fakeMedia),
  });
  t.after(async () => {
    // Closing the window only hides it, so ask the program itself to quit.
    await app.evaluate(({ app: a }) => a.quit()).catch(() => {});
    await app.close().catch(() => {});
  });
  const page = await app.firstWindow();
  const errors = [];
  page.on('pageerror', (err) => errors.push(err.message));
  page.errors = errors;
  if (devices) {
    await page.addInitScript((c) => { window.__fakeConfig = c; }, { devices, camGranted: true, micGranted: true });
    await page.addInitScript({ path: FAKE });
    await page.reload();
  }
  return { app, page };
}

const windowVisible = (app) => app.evaluate(
  ({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].isVisible());

const closeWindow = (app) => app.evaluate(
  ({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].close());

const waitLive = (page) => page.waitForFunction(
  () => document.body.classList.contains('live') && document.getElementById('video').videoWidth > 0, null, WAIT);

const waitMessage = (page, text) => page.waitForFunction(
  (t) => !document.getElementById('screen').hidden
    && document.getElementById('screenMsg').textContent === t, text, WAIT);

const value = (page, id) => page.evaluate((i) => document.getElementById(i).value, id);

test('opens a window and shows the capture card with no Start button or prompt', { skip }, async (t) => {
  const { app, page } = await launch(t);
  await waitLive(page);
  assert.equal(await page.title(), 'Console Viewer');
  assert.equal(await windowVisible(app), true);
  assert.equal(await value(page, 'videoSel'), 'card1');
  assert.equal(await value(page, 'audioSel'), 'card1a');
  assert.equal(await page.evaluate(() => window.isSecureContext), true);
  assert.equal(await page.evaluate(() => typeof window.require), 'undefined', 'the page must not get Node access');
  assert.deepEqual(page.errors, []);
});

test('real device names are readable without opening a camera', { skip }, async (t) => {
  // The background mode depends on this: it must spot a capture card by its
  // name while everything is closed. This one test looks at the real devices
  // on this PC, but only lists them. Nothing is opened.
  const { page } = await launch(t, { args: ['--hidden'], devices: null, fakeMedia: false });
  await waitMessage(page, 'Paused');
  const cameras = await page.evaluate(async () => (await navigator.mediaDevices.enumerateDevices())
    .filter((d) => d.kind === 'videoinput').map((d) => ({ label: d.label, hasId: !!d.deviceId })));
  if (!cameras.length) { t.skip('this PC has no camera to list'); return; }
  assert.ok(cameras.every((c) => c.label && c.hasId), `got ${JSON.stringify(cameras)}`);
  assert.equal(await page.evaluate(() => document.getElementById('video').srcObject), null);
});

test('closing the window hides it to the tray and lets go of the device', { skip }, async (t) => {
  const { app, page } = await launch(t);
  await waitLive(page);
  await closeWindow(app);
  await waitMessage(page, 'Paused');
  assert.equal(await windowVisible(app), false);
  assert.equal(await app.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows().length), 1, 'still running');
  assert.equal(await page.evaluate(() => window.__fake.liveTrackCount()), 0);
});

test('started with --hidden: stays hidden, then opens when a capture card is plugged in', { skip }, async (t) => {
  const { app, page } = await launch(t, { args: ['--hidden'], devices: [WEBCAM, MIC] });
  await waitMessage(page, 'Paused');
  await page.waitForTimeout(1000);
  assert.equal(await windowVisible(app), false);
  assert.equal(await page.evaluate(() => window.__fake.state.calls.length), 0, 'the webcam must not be opened');

  await page.evaluate(([card, sound]) => window.__fake.plug(card, sound), [CARD, CARD_SOUND]);
  await waitLive(page);
  assert.equal(await windowVisible(app), true);
  assert.equal(await value(page, 'videoSel'), 'card1');
  assert.equal(await value(page, 'audioSel'), 'card1a');
});

test('clicking the tray icon brings the window back and reconnects', { skip }, async (t) => {
  const { app, page } = await launch(t);
  await waitLive(page);
  await closeWindow(app);
  await waitMessage(page, 'Paused');
  // A second launch and a tray click both go through the same "show" path.
  await app.evaluate(({ app: a }) => a.emit('second-instance'));
  await waitLive(page);
  assert.equal(await windowVisible(app), true);
});

test('a show request that arrives while the page is loading is not lost', { skip }, async (t) => {
  const { app, page } = await launch(t, { args: ['--hidden'] });
  await waitMessage(page, 'Paused');
  // Ask for the window, then reload: the page comes up believing it should
  // stay paused, and only the shell knows better.
  await app.evaluate(({ app: a }) => a.emit('second-instance'));
  await waitLive(page);
  await page.reload();
  await waitLive(page);
  assert.equal(await windowVisible(app), true);
});

test('quitting while recording saves the clip first', { skip }, async (t) => {
  const { app, page } = await launch(t);
  const folder = fs.mkdtempSync(path.join(profile, 'clips-'));
  await app.evaluate(({ app: a }, dir) => a.setPath('downloads', dir), folder);
  await waitLive(page);
  await page.click('#recBtn');
  await page.waitForTimeout(2500);
  const exited = new Promise((resolve) => app.process().once('exit', resolve));
  await app.evaluate(({ app: a }) => { a.quit(); }).catch(() => {});
  await exited;
  const clips = fs.readdirSync(folder).filter((f) => /^console-viewer-.*\.(mp4|webm)$/.test(f));
  assert.equal(clips.length, 1, `expected one clip, found ${JSON.stringify(fs.readdirSync(folder))}`);
  const clipSize = fs.statSync(path.join(folder, clips[0])).size;
  // A real video file, not an empty or cut-off one. (This encoder makes very
  // small files from the simple test picture, so size alone proves little.)
  assert.ok(clipSize > 2000, `clip ${clips[0]} is only ${clipSize} bytes`);
  const head = fs.readFileSync(path.join(folder, clips[0])).subarray(0, 64).toString('latin1');
  assert.match(head, /ftyp|Eß£/, 'the file should start like an MP4 or WebM video');
});

test('screenshots are saved to Downloads without asking where', { skip }, async (t) => {
  const { app, page } = await launch(t);
  const folder = fs.mkdtempSync(path.join(profile, 'downloads-'));
  await app.evaluate(({ app: a }, dir) => a.setPath('downloads', dir), folder);
  await waitLive(page);
  await page.keyboard.press('s');
  let saved = false;
  for (let i = 0; i < 50 && !saved; i += 1) {
    const files = fs.readdirSync(folder).filter((f) => /^console-viewer-.*\.png$/.test(f));
    saved = files.length > 0 && fs.statSync(path.join(folder, files[0])).size > 1000;
    if (!saved) await new Promise((r) => setTimeout(r, 200));
  }
  assert.ok(saved, 'no screenshot appeared in the downloads folder');
});

test('starting it a second time does not open a second copy', { skip }, async (t) => {
  const userData = fs.mkdtempSync(path.join(profile, 'shared-'));
  const { app, page } = await launch(t, { userData });
  await waitLive(page);
  await closeWindow(app);
  await waitMessage(page, 'Paused');

  const second = spawn(require(ELECTRON_MODULE), [DESKTOP_DIR], { env: testEnv(userData), stdio: 'ignore' });
  const code = await new Promise((resolve) => second.on('exit', resolve));
  assert.equal(code, 0, 'the second copy should exit by itself');
  await waitLive(page); // and the first one comes back to the front
  assert.equal(await windowVisible(app), true);
});

// Only runs after `npm run dist` (or `npm run pack`) in the desktop folder.
const PACKAGED = path.join(DESKTOP_DIR, 'dist', 'win-unpacked', 'Console Viewer.exe');
const skipPackaged = fs.existsSync(PACKAGED) ? false : 'build the app first with "npm run dist" in the desktop folder';

test('the built program starts and shows the capture card', { skip: skipPackaged }, async (t) => {
  const { app, page } = await launch(t, { executablePath: PACKAGED, appArgs: [] });
  await waitLive(page);
  assert.equal(await app.evaluate(({ app: a }) => a.isPackaged), true);
  assert.equal(await page.title(), 'Console Viewer');
  assert.equal(await value(page, 'videoSel'), 'card1');
  // The tray's "Start with Windows" switch must be usable in the built app.
  assert.equal(await app.evaluate(({ app: a }) => typeof a.getLoginItemSettings().openAtLogin), 'boolean');
});
