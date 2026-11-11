/*
 * Starts the desktop app during development: `npm start`.
 *
 * Terminals opened by some editors set ELECTRON_RUN_AS_NODE, which makes
 * Electron run as plain Node and never open a window. Clear it first.
 */
'use strict';

const { spawn } = require('node:child_process');
const path = require('node:path');

const env = { ...process.env };
delete env.ELECTRON_RUN_AS_NODE;

const child = spawn(require('electron'), [path.join(__dirname, '..'), ...process.argv.slice(2)], {
  env,
  stdio: 'inherit',
});
child.on('exit', (code) => process.exit(code ?? 0));
