/*
 * Fake cameras and microphones for the tests.
 *
 * Loaded into the page before app.js. It replaces the browser's device list
 * and getUserMedia with a pretend set of devices the test can plug in, unplug,
 * make busy, or turn black. It copies how Chrome really behaves: device names
 * and ids stay hidden until access has been granted.
 *
 * Config comes from window.__fakeConfig:
 *   { devices: [...], camGranted, micGranted, camDenied, micDenied, busy: [ids] }
 * Each device: { kind: 'videoinput' | 'audioinput', deviceId, label, groupId, black }
 */
(() => {
  'use strict';

  const cfg = window.__fakeConfig || {};
  const state = {
    devices: (cfg.devices || []).map((d) => ({ ...d })),
    camGranted: !!cfg.camGranted,
    micGranted: !!cfg.micGranted,
    camDenied: !!cfg.camDenied,
    micDenied: !!cfg.micDenied,
    busy: new Set(cfg.busy || []),
    live: [],   // { deviceId, track, cleanup }
    calls: [],  // every getUserMedia request, for the tests to inspect
  };

  const md = navigator.mediaDevices;
  const fail = (name, message) => Promise.reject(new DOMException(message || name, name));
  const ofKind = (kind) => state.devices.filter((d) => d.kind === kind);

  let audioContext = null;

  function makeVideoTrack(device) {
    const canvas = document.createElement('canvas');
    canvas.width = 1280;
    canvas.height = 720;
    const ctx = canvas.getContext('2d');
    const draw = () => {
      ctx.fillStyle = device.black ? '#000' : '#2a8f6a';
      ctx.fillRect(0, 0, canvas.width, canvas.height);
      if (!device.black) {
        ctx.fillStyle = '#fff';
        ctx.fillRect((Date.now() / 5) % canvas.width, 200, 120, 120);
      }
    };
    draw();
    const timer = setInterval(draw, 40);
    const track = canvas.captureStream(30).getVideoTracks()[0];
    return { track, cleanup: () => clearInterval(timer) };
  }

  function makeAudioTrack() {
    audioContext = audioContext || new AudioContext();
    const track = audioContext.createMediaStreamDestination().stream.getAudioTracks()[0];
    return { track, cleanup: () => {} };
  }

  // Works out which device a getUserMedia constraint is asking for.
  function resolve(kind, constraint) {
    const list = ofKind(kind);
    const wanted = constraint && constraint.deviceId
      && (constraint.deviceId.exact || constraint.deviceId);
    if (typeof wanted === 'string' && wanted) {
      const found = list.find((d) => d.deviceId === wanted);
      return found ? { device: found } : { error: 'OverconstrainedError' };
    }
    return list.length ? { device: list[0] } : { error: 'NotFoundError' };
  }

  md.getUserMedia = (constraints = {}) => {
    state.calls.push(JSON.parse(JSON.stringify(constraints)));
    const wantVideo = !!constraints.video;
    const wantAudio = !!constraints.audio;
    if (wantVideo && state.camDenied) return fail('NotAllowedError');
    if (wantAudio && state.micDenied) return fail('NotAllowedError');

    const picks = [];
    for (const [want, kind, constraint] of [
      [wantVideo, 'videoinput', constraints.video],
      [wantAudio, 'audioinput', constraints.audio],
    ]) {
      if (!want) continue;
      const { device, error } = resolve(kind, constraint);
      if (error) return fail(error);
      picks.push(device);
    }

    // As in a real browser, access is granted before the device is opened,
    // so a busy device fails after the names have already been unlocked.
    if (wantVideo) state.camGranted = true;
    if (wantAudio) state.micGranted = true;
    if (picks.some((device) => state.busy.has(device.deviceId))) {
      return fail('NotReadableError', 'Device in use');
    }

    const tracks = picks.map((device) => {
      const made = device.kind === 'videoinput' ? makeVideoTrack(device) : makeAudioTrack();
      state.live.push({ deviceId: device.deviceId, ...made });
      return made.track;
    });
    return Promise.resolve(new MediaStream(tracks));
  };

  md.enumerateDevices = () => {
    const out = [];
    for (const [kind, granted] of [['videoinput', state.camGranted], ['audioinput', state.micGranted]]) {
      const list = ofKind(kind);
      if (!list.length) continue;
      if (granted) {
        list.forEach((d) => out.push({ kind, deviceId: d.deviceId, label: d.label, groupId: d.groupId || '' }));
      } else {
        // Access not granted yet: one anonymous entry per kind, as Chrome does.
        out.push({ kind, deviceId: '', label: '', groupId: '' });
      }
    }
    return Promise.resolve(out);
  };

  if (navigator.permissions) {
    navigator.permissions.query = ({ name }) => Promise.resolve({
      state: name === 'camera' && state.camGranted ? 'granted' : 'prompt',
    });
  }

  const announce = () => md.dispatchEvent(new Event('devicechange'));

  window.__fake = {
    state,
    plug(...devices) {
      devices.forEach((d) => state.devices.push({ ...d }));
      announce();
    },
    unplug(...ids) {
      state.devices = state.devices.filter((d) => !ids.includes(d.deviceId));
      state.live = state.live.filter((entry) => {
        if (!ids.includes(entry.deviceId)) return true;
        entry.cleanup();
        if (entry.track.readyState === 'live') {
          entry.track.stop();
          entry.track.dispatchEvent(new Event('ended'));
        }
        return false;
      });
      announce();
    },
    setBlack(id, black) {
      const device = state.devices.find((d) => d.deviceId === id);
      if (device) device.black = black;
    },
    setBusy(id, busy) {
      if (busy) state.busy.add(id); else state.busy.delete(id);
    },
    // Tracks handed out that the app has not stopped. Should never grow.
    liveTrackCount() {
      return state.live.filter((entry) => entry.track.readyState === 'live').length;
    },
  };
})();
