/*
 * Console Viewer — desktop app.
 *
 * A thin shell around the web app in ../web. It adds what a browser tab
 * cannot do: live in the system tray, come to the front by itself when a
 * capture card is plugged in, and start with Windows.
 */
'use strict';

const {
  app, BrowserWindow, Tray, Menu, nativeImage, ipcMain, protocol, net, session, powerSaveBlocker,
} = require('electron');
const path = require('node:path');
const fs = require('node:fs');
const { pathToFileURL } = require('node:url');

// The web app is shipped next to the program when installed, and read
// straight from the repository folder during development.
const WEB_DIR = app.isPackaged
  ? path.join(process.resourcesPath, 'web')
  : path.join(__dirname, '..', 'web');
const ICON = path.join(__dirname, 'assets', 'icon.png');
const HIDDEN_FLAG = '--hidden';
const startHidden = process.argv.includes(HIDDEN_FLAG);

// Tests set these so nothing touches the real webcam or the real profile.
if (process.env.CV_FAKE_MEDIA === '1') app.commandLine.appendSwitch('use-fake-device-for-media-stream');
if (process.env.CV_USER_DATA) app.setPath('userData', process.env.CV_USER_DATA);

let win = null;
let tray = null;
let quitting = false;
let trayTipShown = false;
let sleepBlocker = null;

// app:// behaves like https, which the browser engine requires for cameras.
protocol.registerSchemesAsPrivileged([
  { scheme: 'app', privileges: { standard: true, secure: true, supportFetchAPI: true, stream: true } },
]);

function serveWebApp() {
  protocol.handle('app', (request) => {
    const { pathname } = new URL(request.url);
    const relative = decodeURIComponent(pathname === '/' ? '/index.html' : pathname);
    const file = path.normalize(path.join(WEB_DIR, relative));
    if (!file.startsWith(WEB_DIR + path.sep)) return new Response('Forbidden', { status: 403 });
    return net.fetch(pathToFileURL(file).toString());
  });
}

function setUpPermissions() {
  // The app's only job is to open a capture card, so allow that and nothing else.
  const allowed = new Set(['media', 'fullscreen']);
  session.defaultSession.setPermissionRequestHandler((_wc, permission, callback) => {
    callback(allowed.has(permission));
  });
  session.defaultSession.setPermissionCheckHandler((_wc, permission) => allowed.has(permission));
}

// Screenshots and clips go straight to Downloads, with no "Save as" window.
function setUpDownloads() {
  session.defaultSession.on('will-download', (_event, item) => {
    const folder = app.getPath('downloads');
    const parsed = path.parse(path.basename(item.getFilename()));
    let target = path.join(folder, parsed.base);
    for (let n = 1; fs.existsSync(target); n += 1) {
      target = path.join(folder, `${parsed.name}-${n}${parsed.ext}`);
    }
    item.setSavePath(target);
  });
}

function createWindow() {
  win = new BrowserWindow({
    width: 1280,
    height: 760,
    minWidth: 480,
    minHeight: 320,
    show: false,
    title: 'Console Viewer',
    icon: ICON,
    backgroundColor: '#0b0d10',
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      sandbox: true,
      nodeIntegration: false,
      backgroundThrottling: false, // keep noticing new devices while hidden
      additionalArguments: startHidden ? ['--cv-start-paused'] : [],
    },
  });

  // The window only ever shows our own page.
  win.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  win.webContents.on('will-navigate', (event, url) => {
    if (!url.startsWith('app://viewer/')) event.preventDefault();
  });

  // Closing the window keeps the app in the tray, waiting for a capture card.
  win.on('close', (event) => {
    if (quitting) return;
    event.preventDefault();
    hideWindow();
  });
  win.on('closed', () => { win = null; });

  win.once('ready-to-show', () => { if (!startHidden) win.show(); });
  win.loadURL('app://viewer/');
}

function showWindow() {
  if (!win) return;
  const wasHidden = !win.isVisible();
  if (win.isMinimized()) win.restore();
  win.show();
  win.focus();
  if (wasHidden) win.webContents.send('host:resume');
}

function hideWindow() {
  if (!win) return;
  if (win.isFullScreen()) win.setFullScreen(false);
  win.hide();
  win.webContents.send('host:pause');
  if (!trayTipShown && tray && process.platform === 'win32') {
    trayTipShown = true;
    tray.displayBalloon({
      title: 'Console Viewer is still running',
      content: 'It will open by itself when you plug in a capture card. Right-click the tray icon to quit.',
      icon: nativeImage.createFromPath(ICON),
    });
  }
}

function startsWithWindows() {
  return app.getLoginItemSettings({ args: [HIDDEN_FLAG] }).openAtLogin;
}

function buildTrayMenu() {
  return Menu.buildFromTemplate([
    { label: 'Open Console Viewer', click: showWindow },
    {
      label: 'Start with Windows',
      type: 'checkbox',
      checked: app.isPackaged && startsWithWindows(),
      enabled: app.isPackaged, // only meaningful for the installed app
      click: (item) => {
        app.setLoginItemSettings({ openAtLogin: item.checked, args: [HIDDEN_FLAG] });
      },
    },
    { type: 'separator' },
    { label: 'Quit', click: () => { quitting = true; app.quit(); } },
  ]);
}

function createTray() {
  const image = nativeImage.createFromPath(ICON).resize({ width: 16, height: 16 });
  tray = new Tray(image);
  tray.setToolTip('Console Viewer');
  tray.setContextMenu(buildTrayMenu());
  tray.on('click', showWindow);
}

// Messages from the page. Ignore anything that is not our own window.
const fromOurWindow = (event) => !!win && event.sender === win.webContents;

ipcMain.on('host:capture-connected', (event) => {
  if (fromOurWindow(event)) showWindow();
});

ipcMain.on('host:playing', (event, playing) => {
  if (!fromOurWindow(event)) return;
  // A controller sends no keyboard or mouse input, so stop Windows from
  // dimming or locking the screen while a picture is showing.
  if (playing && sleepBlocker === null) {
    sleepBlocker = powerSaveBlocker.start('prevent-display-sleep');
  } else if (!playing && sleepBlocker !== null) {
    powerSaveBlocker.stop(sleepBlocker);
    sleepBlocker = null;
  }
});

// Starting the app a second time brings the first one to the front.
if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', showWindow);
  app.on('before-quit', () => { quitting = true; });
  app.on('window-all-closed', () => { /* stay in the tray */ });

  app.whenReady().then(() => {
    Menu.setApplicationMenu(null);
    serveWebApp();
    setUpPermissions();
    setUpDownloads();
    createTray();
    createWindow();
  });
}
