/*
 * The only bridge between the web page and the desktop shell.
 * The page sees a small object, window.consoleViewerHost, and nothing else.
 */
'use strict';

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('consoleViewerHost', {
  // True when the app was started in the background (with Windows).
  startPaused: process.argv.includes('--cv-start-paused'),
  // The window was hidden to the tray: release the camera.
  onPause: (callback) => { ipcRenderer.on('host:pause', () => callback()); },
  // The window is back: connect again.
  onResume: (callback) => { ipcRenderer.on('host:resume', () => callback()); },
  // A capture card was plugged in while hidden: ask for the window to open.
  captureCardConnected: (label) => { ipcRenderer.send('host:capture-connected', String(label || '')); },
  // A picture is showing (or stopped): keep the screen awake (or let it sleep).
  setPlaying: (playing) => { ipcRenderer.send('host:playing', !!playing); },
});
