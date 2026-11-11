/*
 * Browser tests for the web app, using pretend capture cards.
 *
 * Runs the real page in Microsoft Edge (already on every Windows PC) with
 * tests/fake-devices.js standing in for the hardware.
 *
 *   cd tests
 *   npm install
 *   npm test
 */
'use strict';

const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright-core');

const WEB_DIR = path.resolve(__dirname, '..', 'web');
const FAKE = path.join(__dirname, 'fake-devices.js');
const TYPES = { '.html': 'text/html', '.css': 'text/css', '.js': 'text/javascript' };

// Pretend hardware.
const WEBCAM = { kind: 'videoinput', deviceId: 'cam1', label: 'HP True Vision HD Camera', groupId: 'g-cam' };
const MIC = { kind: 'audioinput', deviceId: 'mic1', label: 'Microphone Array', groupId: 'g-mic' };
const CARD = { kind: 'videoinput', deviceId: 'card1', label: 'USB3.0 HD Video Capture', groupId: 'g-card' };
const CARD_SOUND = { kind: 'audioinput', deviceId: 'card1a', label: 'Digital Audio Interface (USB Digital Audio)', groupId: 'g-card' };
const CARD2 = { kind: 'videoinput', deviceId: 'card2', label: 'Elgato HD60 X', groupId: 'g-card2' };
// A webcam whose name contains "UVC" and that has its own microphone.
const UVC_WEBCAM = { kind: 'videoinput', deviceId: 'cam2', label: 'USB2.0 HD UVC WebCam', groupId: 'g-cam2' };
const UVC_WEBCAM_MIC = { kind: 'audioinput', deviceId: 'cam2a', label: 'Microphone (USB2.0 HD UVC WebCam)', groupId: 'g-cam2' };

let server;
let base;
let browser;

before(async () => {
  server = http.createServer((req, res) => {
    const name = req.url.split('?')[0];
    const file = path.join(WEB_DIR, name === '/' ? 'index.html' : name);
    if (!file.startsWith(WEB_DIR)) { res.writeHead(403).end(); return; }
    fs.readFile(file, (err, data) => {
      if (err) { res.writeHead(404).end(); return; }
      res.writeHead(200, { 'content-type': TYPES[path.extname(file)] || 'application/octet-stream' });
      res.end(data);
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  base = `http://localhost:${server.address().port}/`;
  browser = await chromium.launch({
    channel: 'msedge',
    headless: true,
    args: ['--autoplay-policy=no-user-gesture-required'],
  });
});

after(async () => {
  if (browser) await browser.close();
  if (server) await new Promise((resolve) => server.close(resolve));
});

// Opens the app with the given pretend devices. `saved` pre-fills the
// choices the app remembers between visits.
// `host` pretends the page is inside the desktop app: { startPaused }.
// `init` is extra page code to run before the app starts.
async function openApp(t, config, saved, host, init) {
  const context = await browser.newContext();
  t.after(() => context.close());
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', (err) => errors.push(err.message));
  await page.addInitScript((c) => { window.__fakeConfig = c; }, config);
  if (saved) {
    await page.addInitScript((s) => {
      Object.entries(s).forEach(([key, value]) => localStorage.setItem('cv:' + key, value));
    }, saved);
  }
  if (host) {
    await page.addInitScript((h) => {
      window.__hostCalls = [];
      window.__hostCallbacks = {};
      window.consoleViewerHost = {
        startPaused: !!h.startPaused,
        onPause: (cb) => { window.__hostCallbacks.pause = cb; },
        onResume: (cb) => { window.__hostCallbacks.resume = cb; },
        captureCardConnected: (label) => { window.__hostCalls.push(['card', label]); },
        setPlaying: (on) => { window.__hostCalls.push(['playing', on]); },
      };
    }, host);
  }
  await page.addInitScript({ path: FAKE });
  if (init) await page.addInitScript(init);
  await page.goto(base);
  page.errors = errors;
  return page;
}

const WAIT = { timeout: 8000 };

const waitLive = (page) => page.waitForFunction(
  () => document.body.classList.contains('live') && document.getElementById('video').videoWidth > 0, null, WAIT);

const waitMessage = (page, text) => page.waitForFunction(
  (t) => !document.getElementById('screen').hidden
    && document.getElementById('screenMsg').textContent === t, text, WAIT);

const waitValue = (page, id, value) => page.waitForFunction(
  ([i, v]) => document.getElementById(i).value === v, [id, value], WAIT);

const value = (page, id) => page.evaluate((i) => document.getElementById(i).value, id);
const fake = (page, fn, ...args) => page.evaluate(([f, a]) => window.__fake[f](...a), [fn, args]);

test('webcam only: shows the picture and never plays the microphone', async (t) => {
  const page = await openApp(t, { devices: [WEBCAM, MIC] });
  await page.click('#startBtn');
  await waitLive(page);
  assert.equal(await value(page, 'videoSel'), 'cam1');
  assert.equal(await value(page, 'audioSel'), '', 'sound must stay off for a webcam');
  assert.equal(await page.isDisabled('#muteBtn'), true);
  assert.equal(await fake(page, 'liveTrackCount'), 1, 'only the picture track should be open');
  assert.deepEqual(page.errors, []);
});

test('capture card is preferred over the webcam, with its own sound', async (t) => {
  const page = await openApp(t, { devices: [WEBCAM, MIC, CARD, CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  assert.equal(await value(page, 'videoSel'), 'card1');
  assert.equal(await value(page, 'audioSel'), 'card1a');
  assert.equal(await page.isDisabled('#muteBtn'), false);
  assert.equal(await fake(page, 'liveTrackCount'), 2, 'probe streams must be closed');
  const text = await page.textContent('#stats');
  assert.match(text, /1280×720/);
  assert.deepEqual(page.errors, []);
});

test('game sound is opened with voice processing switched off', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  const calls = await page.evaluate(() => window.__fake.state.calls);
  const last = calls[calls.length - 1];
  assert.equal(last.audio.echoCancellation, false);
  assert.equal(last.audio.noiseSuppression, false);
  assert.equal(last.audio.autoGainControl, false);
  assert.equal(last.video.deviceId.exact, 'card1');
});

test('a webcam with "UVC" in its name does not get its microphone played', async (t) => {
  const page = await openApp(t, { devices: [UVC_WEBCAM, UVC_WEBCAM_MIC] });
  await page.click('#startBtn');
  await waitLive(page);
  assert.equal(await value(page, 'audioSel'), '');
});

test('first visit: Start pressed before the card is plugged in, then it connects', async (t) => {
  const page = await openApp(t, { devices: [] });
  await page.click('#startBtn');
  await waitMessage(page, 'No video device found');
  await fake(page, 'plug', CARD, CARD_SOUND);
  await waitLive(page);
  assert.equal(await value(page, 'videoSel'), 'card1');
  assert.equal(await value(page, 'audioSel'), 'card1a');
});

test('access granted on an earlier visit: starts without pressing Start', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND], camGranted: true, micGranted: true });
  await waitLive(page);
  assert.equal(await value(page, 'videoSel'), 'card1');
});

test('unplug, wait, plug back in: reconnects', async (t) => {
  const page = await openApp(t, { devices: [WEBCAM, MIC, CARD, CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  await fake(page, 'unplug', 'card1', 'card1a');
  await waitMessage(page, 'Device disconnected');
  await page.waitForTimeout(1500); // long enough for the app to notice it has gone
  assert.equal(await page.isVisible('#screen'), true, 'must not fall back to the webcam by itself');
  await fake(page, 'plug', CARD, CARD_SOUND);
  await waitLive(page);
  assert.equal(await value(page, 'videoSel'), 'card1');
  assert.equal(await value(page, 'audioSel'), 'card1a');
});

test('quick unplug and replug: reconnects', async (t) => {
  const page = await openApp(t, { devices: [WEBCAM, MIC, CARD, CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  await fake(page, 'unplug', 'card1', 'card1a');
  await waitMessage(page, 'Device disconnected');
  assert.equal(await fake(page, 'liveTrackCount'), 0, 'nothing should be left running');
  await fake(page, 'plug', CARD, CARD_SOUND);
  await waitLive(page);
  assert.equal(await value(page, 'videoSel'), 'card1');
  assert.equal(await value(page, 'audioSel'), 'card1a');
  assert.deepEqual(page.errors, []);
});

test('a hiccup in the device list does not stop a working picture', async (t) => {
  const page = await openApp(t, { devices: [WEBCAM, MIC, CARD, CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  await fake(page, 'dropFromList', 'card1', 'card1a');
  await page.waitForTimeout(1500);
  assert.equal(await page.evaluate(() => document.body.classList.contains('live')), true);
  assert.equal(await fake(page, 'liveTrackCount'), 2);
});

test('plugging in a capture card takes over from the webcam', async (t) => {
  const page = await openApp(t, { devices: [WEBCAM, MIC] });
  await page.click('#startBtn');
  await waitLive(page);
  await fake(page, 'plug', CARD, CARD_SOUND);
  await waitValue(page, 'videoSel', 'card1');
  await waitLive(page);
  assert.equal(await value(page, 'audioSel'), 'card1a');
  assert.equal(await fake(page, 'liveTrackCount'), 2, 'the webcam stream must be closed');
});

test('a second capture card does not interrupt the one being watched', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  await fake(page, 'plug', CARD2);
  await page.waitForFunction(() => document.getElementById('videoSel').options.length === 2, null, WAIT);
  assert.equal(await value(page, 'videoSel'), 'card1');
  assert.equal(await page.evaluate(() => document.body.classList.contains('live')), true);
});

test('sound device that registers late is picked up', async (t) => {
  const page = await openApp(t, { devices: [], camGranted: true, micGranted: true });
  await waitMessage(page, 'No video device found');
  await fake(page, 'plug', CARD);
  await waitLive(page);
  assert.equal(await value(page, 'audioSel'), '');
  await fake(page, 'plug', CARD_SOUND);
  await waitValue(page, 'audioSel', 'card1a');
  await waitLive(page);
});

test('choosing "No sound" is remembered and not overridden', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  await page.selectOption('#audioSel', '');
  await waitValue(page, 'audioSel', '');
  await waitLive(page);
  await fake(page, 'plug', MIC); // any device change must not switch sound back on
  await page.waitForFunction(() => document.getElementById('audioSel').options.length === 3, null, WAIT);
  assert.equal(await value(page, 'audioSel'), '');
  assert.equal(await fake(page, 'liveTrackCount'), 1);
});

test('remembered device is busy: falls back to the next one', async (t) => {
  const page = await openApp(t, { devices: [WEBCAM, MIC, CARD, CARD_SOUND], busy: ['cam1'] }, { video: 'cam1' });
  await page.click('#startBtn');
  await waitLive(page);
  assert.equal(await value(page, 'videoSel'), 'card1');
});

test('every device busy: clear message with a way to retry', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND], busy: ['card1'], camGranted: true, micGranted: true });
  await waitMessage(page, 'The device is busy');
  assert.equal(await page.isVisible('#startBtn'), true);
  await fake(page, 'setBusy', 'card1', false);
  await page.click('#startBtn');
  await waitLive(page);
});

test('camera access refused: explains what to do', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND], camDenied: true });
  await page.click('#startBtn');
  await waitMessage(page, 'Camera access was blocked');
  assert.equal(await page.isVisible('#startBtn'), true);
});

test('microphone refused: picture still works', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND], micDenied: true });
  await page.click('#startBtn');
  await waitLive(page);
  assert.equal(await value(page, 'videoSel'), 'card1');
  assert.equal(await value(page, 'audioSel'), '');
});

test('black picture brings up the HDCP tip, and it goes away when the picture returns', async (t) => {
  const page = await openApp(t, { devices: [{ ...CARD, black: true }, CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  assert.equal(await page.isVisible('#hint'), false, 'no tip in the first seconds');
  await page.waitForFunction(() => !document.getElementById('hint').hidden, null, { timeout: 20000 });
  await fake(page, 'setBlack', 'card1', false);
  await page.waitForFunction(() => document.getElementById('hint').hidden, null, WAIT);
});

test('a normal picture never shows the HDCP tip', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  await page.waitForTimeout(12000);
  assert.equal(await page.isVisible('#hint'), false);
});

test('control bar hides when the mouse is still and returns on movement', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  await page.mouse.move(400, 400);
  await page.waitForFunction(() => document.body.classList.contains('idle'), null, WAIT);
  await page.mouse.move(420, 420);
  await page.waitForFunction(() => !document.body.classList.contains('idle'), null, WAIT);
});

test('a menu left focused does not jam the bar or the shortcuts', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  await page.focus('#qualitySel');
  await page.mouse.move(400, 400);
  await page.waitForFunction(() => document.body.classList.contains('idle'), null, { timeout: 15000 });
  await page.keyboard.press('z');
  assert.equal(await page.evaluate(() => document.getElementById('video').classList.contains('fill')), true);
});

test('keyboard shortcuts: Z fills the screen, M mutes', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  await page.mouse.move(400, 400);
  await page.keyboard.press('z');
  assert.equal(await page.evaluate(() => document.getElementById('video').classList.contains('fill')), true);
  const before = await page.evaluate(() => document.getElementById('video').muted);
  await page.keyboard.press('m');
  assert.equal(await page.evaluate(() => document.getElementById('video').muted), !before);
  assert.equal(await page.textContent('#muteBtn'), before ? 'Mute' : 'Unmute');
});

test('switching quality reopens the same device at the new size', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  await page.selectOption('#qualitySel', '720');
  await page.waitForFunction(() => {
    const calls = window.__fake.state.calls;
    const last = calls[calls.length - 1];
    return last.video && last.video.width && last.video.width.ideal === 1280;
  }, null, WAIT);
  await waitLive(page);
  assert.equal(await value(page, 'videoSel'), 'card1');
  assert.equal(await fake(page, 'liveTrackCount'), 2, 'the old stream must be closed');
});

test('screenshot downloads a PNG', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  const [download] = await Promise.all([
    page.waitForEvent('download', WAIT),
    page.click('#shotBtn'),
  ]);
  assert.match(download.suggestedFilename(), /^console-viewer-\d{8}-\d{6}\.png$/);
});

// ---------- Recording clips ----------

const isRecording = (page) => page.evaluate(() => !document.getElementById('recBadge').hidden);

async function savedSize(download) {
  return fs.statSync(await download.path()).size;
}

test('R records a clip and saves it as a video file', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  await page.mouse.move(400, 400);
  await page.keyboard.press('r');
  assert.equal(await isRecording(page), true);
  assert.equal(await page.textContent('#recBtn'), 'Stop recording');
  await page.waitForTimeout(2500);
  const [download] = await Promise.all([
    page.waitForEvent('download', WAIT),
    page.keyboard.press('r'),
  ]);
  assert.match(download.suggestedFilename(), /^console-viewer-\d{8}-\d{6}\.(mp4|webm)$/);
  assert.ok(await savedSize(download) > 10000, 'the clip should contain real video');
  assert.equal(await isRecording(page), false);
  assert.equal(await page.textContent('#recBtn'), 'Record');
  assert.match(await page.textContent('#recTime'), /^0:0\d$/);
  assert.deepEqual(page.errors, []);
});

test('unplugging while recording still saves the clip', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  await page.click('#recBtn');
  await page.waitForTimeout(2500);
  const [download] = await Promise.all([
    page.waitForEvent('download', WAIT),
    fake(page, 'unplug', 'card1', 'card1a'),
  ]);
  assert.ok(await savedSize(download) > 10000);
  await waitMessage(page, 'Device disconnected');
  assert.equal(await isRecording(page), false);
  assert.deepEqual(page.errors, []);
});

test('changing quality while recording saves the clip and stops recording', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  await page.click('#recBtn');
  await page.waitForTimeout(2500);
  const [download] = await Promise.all([
    page.waitForEvent('download', WAIT),
    page.selectOption('#qualitySel', '720'),
  ]);
  assert.ok(await savedSize(download) > 10000);
  await waitLive(page);
  assert.equal(await isRecording(page), false);
});

test('recording a picture with no sound works', async (t) => {
  const page = await openApp(t, { devices: [WEBCAM, MIC] });
  await page.click('#startBtn');
  await waitLive(page);
  await page.click('#recBtn');
  await page.waitForTimeout(2000);
  const [download] = await Promise.all([
    page.waitForEvent('download', WAIT),
    page.click('#recBtn'),
  ]);
  assert.ok(await savedSize(download) > 10000);
});

// ---------- Problems found in review ----------

// The most common cheap capture card. Its picture and sound do not share a group.
const CHEAP_CARD = { kind: 'videoinput', deviceId: 'ms1', label: 'USB Video (534d:2109)', groupId: 'g-ms-video' };
const CHEAP_CARD_SOUND = { kind: 'audioinput', deviceId: 'ms1a', label: 'Digital Audio Interface (USB Digital Audio)', groupId: 'g-ms-audio' };

test('cheap card whose sound is not grouped with its picture still gets sound', async (t) => {
  const page = await openApp(t, { devices: [WEBCAM, MIC, CHEAP_CARD, CHEAP_CARD_SOUND] });
  await page.click('#startBtn');
  await waitLive(page);
  assert.equal(await value(page, 'videoSel'), 'ms1');
  assert.equal(await value(page, 'audioSel'), 'ms1a');
});

test('sound device that registers while the picture is still opening is picked up', async (t) => {
  const page = await openApp(t, { devices: [CARD], camGranted: true, micGranted: true, openDelay: 1200 }, null, {});
  await page.waitForFunction(() => window.__fake.state.calls.length > 0, null, WAIT);
  await fake(page, 'plug', CARD_SOUND); // arrives while the picture is opening
  await waitValue(page, 'audioSel', 'card1a');
  await waitLive(page);
  assert.equal(await fake(page, 'liveTrackCount'), 2);
});

test('an interrupted start does not leave the sound muted', async (t) => {
  // The first play() is cut short, as happens when a newer open replaces it.
  const page = await openApp(t, { devices: [CARD, CARD_SOUND] }, null, null, () => {
    const realPlay = HTMLMediaElement.prototype.play;
    let first = true;
    HTMLMediaElement.prototype.play = function play() {
      if (first) {
        first = false;
        return Promise.reject(new DOMException('interrupted', 'AbortError'));
      }
      return realPlay.call(this);
    };
  });
  await page.click('#startBtn');
  await waitLive(page);
  assert.equal(await page.evaluate(() => document.getElementById('video').muted), false);
  assert.equal(await page.textContent('#muteBtn'), 'Mute');
});

test('a browser that refuses to autoplay with sound starts muted and says so', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND] }, null, null, () => {
    const realPlay = HTMLMediaElement.prototype.play;
    HTMLMediaElement.prototype.play = function play() {
      if (!this.muted) return Promise.reject(new DOMException('blocked', 'NotAllowedError'));
      return realPlay.call(this);
    };
  });
  await page.click('#startBtn');
  await waitLive(page);
  assert.equal(await page.evaluate(() => document.getElementById('video').muted), true);
  assert.equal(await page.textContent('#muteBtn'), 'Unmute');
});

test('desktop: a blocked camera points to Windows settings, not a browser address bar', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND], camDenied: true }, null, {});
  await waitMessage(page, 'Camera access was blocked');
  assert.match(await page.textContent('#screenSub'), /Windows Settings/);
});

// ---------- Inside the desktop app ----------

const hostCalls = (page) => page.evaluate(() => window.__hostCalls);
const hostSignal = (page, name) => page.evaluate((n) => window.__hostCallbacks[n](), name);
const GRANTED = { camGranted: true, micGranted: true };

test('desktop: connects without a Start button and reports that a picture is showing', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND], ...GRANTED }, null, {});
  await waitLive(page);
  assert.deepEqual((await hostCalls(page)).pop(), ['playing', true]);
});

test('desktop: started in the background, it opens nothing until a capture card arrives', async (t) => {
  const page = await openApp(t, { devices: [WEBCAM, MIC], ...GRANTED }, null, { startPaused: true });
  await waitMessage(page, 'Paused');
  await page.waitForTimeout(1000);
  assert.equal(await page.evaluate(() => window.__fake.state.calls.length), 0, 'the webcam must not be opened');

  await fake(page, 'plug', UVC_WEBCAM, UVC_WEBCAM_MIC);
  await page.waitForTimeout(1200);
  assert.deepEqual(await hostCalls(page), [], 'a webcam is not a reason to open the window');

  await fake(page, 'plug', CARD, CARD_SOUND);
  await page.waitForFunction(() => window.__hostCalls.some((c) => c[0] === 'card'), null, WAIT);
  assert.deepEqual((await hostCalls(page))[0], ['card', 'USB3.0 HD Video Capture']);
  assert.equal(await page.evaluate(() => window.__fake.state.calls.length), 0, 'still nothing opened while hidden');

  await hostSignal(page, 'resume');
  await waitLive(page);
  assert.equal(await value(page, 'videoSel'), 'card1');
  assert.equal(await value(page, 'audioSel'), 'card1a');
});

test('desktop: hiding the window releases the device, showing it reconnects', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND], ...GRANTED }, null, {});
  await waitLive(page);
  await hostSignal(page, 'pause');
  await waitMessage(page, 'Paused');
  assert.equal(await fake(page, 'liveTrackCount'), 0);
  assert.deepEqual((await hostCalls(page)).pop(), ['playing', false]);
  await hostSignal(page, 'resume');
  await waitLive(page);
  assert.equal(await fake(page, 'liveTrackCount'), 2);
});

test('desktop: hiding the window while recording saves the clip', async (t) => {
  const page = await openApp(t, { devices: [CARD, CARD_SOUND], ...GRANTED }, null, {});
  await waitLive(page);
  await page.click('#recBtn');
  await page.waitForTimeout(2500);
  const [download] = await Promise.all([
    page.waitForEvent('download', WAIT),
    hostSignal(page, 'pause'),
  ]);
  assert.ok(await savedSize(download) > 10000);
});
