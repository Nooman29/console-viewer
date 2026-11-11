/*
 * Console Viewer — web prototype.
 *
 * An HDMI capture card shows up in the browser as a camera plus a microphone.
 * This page opens that camera, plays its sound, and gets out of the way.
 * No dependencies and no build step: open it from a local web server.
 */
(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const video = $('video');
  const screen = $('screen');
  const screenMsg = $('screenMsg');
  const screenSub = $('screenSub');
  const startBtn = $('startBtn');
  const bar = $('bar');
  const videoSel = $('videoSel');
  const audioSel = $('audioSel');
  const qualitySel = $('qualitySel');
  const stats = $('stats');
  const muteBtn = $('muteBtn');
  const volume = $('volume');
  const fillBtn = $('fillBtn');
  const shotBtn = $('shotBtn');
  const recBtn = $('recBtn');
  const recBadge = $('recBadge');
  const recTime = $('recTime');
  const pipBtn = $('pipBtn');
  const fsBtn = $('fsBtn');
  const hint = $('hint');
  const hintClose = $('hintClose');
  const toastEl = $('toast');

  // Names that capture cards commonly report. Used only to prefer a capture
  // card over a webcam when choosing automatically; the user can always pick.
  const CAPTURE_HINT = /capture|hdmi|cam link|elgato|avermedia|live gamer|usb ?3?\.?0? video|macrosilicon|ezcap|video grabber|magewell|ripsaw/i;
  const WEBCAM_HINT = /webcam|camera|facetime|integrated/i;
  const looksLikeCapture = (label) => CAPTURE_HINT.test(label) && !WEBCAM_HINT.test(label);

  const QUALITY = {
    1080: { width: 1920, height: 1080, frameRate: 60 },
    720: { width: 1280, height: 720, frameRate: 60 },
    2160: { width: 3840, height: 2160, frameRate: 60 },
  };

  // localStorage can throw (private windows, blocked site data), so every
  // access is guarded and the page works without it.
  const store = {
    get(key) {
      try { return localStorage.getItem('cv:' + key); } catch { return null; }
    },
    set(key, value) {
      try { localStorage.setItem('cv:' + key, value); } catch { /* ignore */ }
    },
  };

  let stream = null;          // the stream currently on screen, or null
  let started = false;        // true once the user (or saved permission) started us
  let openSeq = 0;            // bumps on every open; stale opens check it and bail
  let videos = [];
  let audios = [];
  let knownVideoIds = new Set();
  let currentVideoId = '';
  let currentAudioId = '';
  let lostVideoId = '';       // the device that was on screen when it disconnected
  let audioAsked = false;
  let wakeLock = null;

  // ---------- Small UI helpers ----------

  function showScreen(message, sub = '', button = '') {
    screenMsg.textContent = message;
    screenSub.textContent = sub;
    startBtn.textContent = button;
    startBtn.hidden = !button;
    screen.hidden = false;
    hint.hidden = true;
    document.body.classList.remove('live', 'idle');
  }

  function hideScreen() {
    screen.hidden = true;
    document.body.classList.add('live');
    wake();
  }

  let toastTimer;
  function toast(text) {
    toastEl.textContent = text;
    toastEl.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => toastEl.classList.remove('show'), 3000);
  }

  function describeError(err) {
    switch (err && err.name) {
      case 'NotAllowedError':
      case 'SecurityError':
        return ['Camera access was blocked',
          'Click the camera icon in the address bar, allow access, then try again.'];
      case 'NotFoundError':
      case 'OverconstrainedError':
        return ['That device is not available',
          'Check the capture card is plugged in, then try again.'];
      case 'NotReadableError':
      case 'AbortError':
        return ['The device is busy',
          'Another app (OBS, Zoom, the Camera app) may be using it. Close that app, then try again.'];
      default:
        return ['Could not open the device', (err && err.message) || 'Unknown error.'];
    }
  }

  function showError(err) {
    console.error(err);
    const [message, sub] = describeError(err);
    showScreen(message, sub, 'Try again');
  }

  // ---------- Devices ----------

  async function listDevices() {
    const all = await navigator.mediaDevices.enumerateDevices();
    videos = all.filter((d) => d.kind === 'videoinput');
    // "default" and "communications" are aliases of real devices; skip them.
    audios = all.filter((d) => d.kind === 'audioinput'
      && d.deviceId !== 'default' && d.deviceId !== 'communications');
  }

  function rememberVideoIds() {
    knownVideoIds = new Set(videos.map((d) => d.deviceId).filter(Boolean));
  }

  // Browsers hide device names until the user grants access, so open a
  // throwaway stream once to unlock them.
  async function ensurePermission() {
    await listDevices();
    if (!videos.length) return;
    const needVideo = !videos.some((d) => d.label);
    const needAudio = audios.length > 0 && !audios.some((d) => d.label);
    const probe = async (constraints) => {
      const s = await navigator.mediaDevices.getUserMedia(constraints);
      s.getTracks().forEach((t) => t.stop());
    };
    let probeError = null;
    if (needVideo) {
      const withAudio = needAudio && !audioAsked;
      audioAsked = true;
      try {
        await probe({ video: true, audio: withAudio });
      } catch (err) {
        probeError = err;
        if (withAudio) { // sound refused or missing: try for the picture alone
          try { await probe({ video: true }); probeError = null; } catch (err2) { probeError = err2; }
        }
      }
    } else if (needAudio && !audioAsked) {
      audioAsked = true;
      try { await probe({ audio: true }); } catch { /* picture only */ }
    } else {
      return;
    }
    await listDevices();
    // The probe opens the default camera. If that one is busy the probe fails
    // even though access was granted, so judge by whether names are visible now.
    if (probeError && !videos.some((d) => d.label)) throw probeError;
  }

  // Every video device, best guess first: the one asked for, the one the user
  // picked last time, anything that looks like a capture card, then the rest.
  function rankVideos(preferId) {
    const saved = store.get('video');
    const score = (d) => (d.deviceId === preferId ? 0
      : d.deviceId === saved ? 1
        : looksLikeCapture(d.label) ? 2 : 3);
    return videos
      .map((d, i) => ({ d, i }))
      .sort((a, b) => score(a.d) - score(b.d) || a.i - b.i)
      .map((x) => x.d);
  }

  // Sound is only picked automatically for capture cards. Auto-playing a
  // webcam's microphone through the speakers would howl with feedback.
  function chooseAudio(videoDevice) {
    const saved = store.get('audio:' + videoDevice.deviceId);
    if (saved === 'none') return '';
    if (saved && audios.some((a) => a.deviceId === saved)) return saved;
    if (!looksLikeCapture(videoDevice.label)) return '';
    const match = audios.find((a) => videoDevice.groupId && a.groupId === videoDevice.groupId)
      || audios.find((a) => looksLikeCapture(a.label));
    return match ? match.deviceId : '';
  }

  function fillSelect(select, devices, selectedId, fallbackName, noneLabel) {
    select.textContent = '';
    if (noneLabel) select.add(new Option(noneLabel, ''));
    devices.forEach((d, i) => {
      if (d.deviceId) select.add(new Option(d.label || `${fallbackName} ${i + 1}`, d.deviceId));
    });
    select.value = selectedId;
  }

  function renderSelects() {
    fillSelect(videoSel, videos, currentVideoId, 'Camera');
    fillSelect(audioSel, audios, currentAudioId, 'Sound input', 'No sound');
  }

  // ---------- Opening and closing the stream ----------

  function stopStream() {
    if (!stream) return;
    stopRecording(); // save what was recorded before the picture goes away
    stream.getTracks().forEach((t) => { t.onended = null; t.stop(); });
    stream = null;
    video.srcObject = null;
    releaseWake();
  }

  // Returns null when the picture is up, 'stale' when a newer open replaced
  // this one, or the error. With quiet set, the caller shows the error.
  async function open(videoId, audioId, quiet = false) {
    const seq = ++openSeq;
    stopStream();
    showScreen('Connecting…');

    const q = QUALITY[qualitySel.value] || QUALITY[1080];
    const videoConstraints = {
      deviceId: { exact: videoId },
      width: { ideal: q.width },
      height: { ideal: q.height },
      frameRate: { ideal: q.frameRate },
    };
    // Voice-call processing would mangle game audio, so switch it all off.
    const audioConstraints = audioId ? {
      deviceId: { exact: audioId },
      echoCancellation: false,
      noiseSuppression: false,
      autoGainControl: false,
      channelCount: { ideal: 2 },
      sampleRate: { ideal: 48000 },
    } : false;

    let s;
    let audioFailed = false;
    try {
      try {
        s = await navigator.mediaDevices.getUserMedia({ video: videoConstraints, audio: audioConstraints });
      } catch (err) {
        if (!audioConstraints) throw err;
        s = await navigator.mediaDevices.getUserMedia({ video: videoConstraints });
        audioFailed = true;
      }
    } catch (err) {
      if (seq !== openSeq) return 'stale';
      if (!quiet) showError(err);
      return err;
    }
    if (seq !== openSeq) { // a newer open started while we waited
      s.getTracks().forEach((t) => t.stop());
      return 'stale';
    }

    stream = s;
    lostVideoId = '';
    currentVideoId = videoId;
    currentAudioId = audioFailed ? '' : audioId;

    const track = s.getVideoTracks()[0];
    try { track.contentHint = 'motion'; } catch { /* optional */ }
    track.onended = () => { if (stream === s) onDisconnect(); };

    video.srcObject = s;
    try {
      await video.play();
    } catch {
      // The browser refused to autoplay with sound: start muted instead.
      video.muted = true;
      try { await video.play(); } catch { /* the autoplay attribute will retry */ }
      toast('Sound is off. Press M to turn it on.');
    }
    if (seq !== openSeq) return 'stale';

    try { await listDevices(); } catch { /* keep the old list */ }
    rememberVideoIds();
    renderSelects();
    updateMuteButton();
    updateStats();
    hideScreen();
    holdWake();
    if (audioFailed) toast('Could not open that sound source. Showing picture only.');
    return null;
  }

  async function start(preferId) {
    started = true;
    const seq = ++openSeq;
    showScreen('Connecting…');
    try {
      await ensurePermission();
    } catch (err) {
      if (seq === openSeq) showError(err);
      return;
    }
    if (seq !== openSeq) return;
    rememberVideoIds();
    if (!videos.length) {
      stopStream();
      showScreen('No video device found',
        'Plug your HDMI capture card into a USB port. It will appear here automatically.', 'Try again');
      return;
    }
    // If the first choice is busy or broken, fall through to the next device
    // instead of leaving the user stuck on an error they cannot get past.
    let firstError = null;
    for (const device of rankVideos(preferId)) {
      if (!device.deviceId) continue;
      const result = await open(device.deviceId, chooseAudio(device), true);
      if (result === null || result === 'stale') return;
      firstError = firstError || result;
    }
    showError(firstError || new Error('No usable video device was found.'));
  }

  function onDisconnect() {
    lostVideoId = currentVideoId;
    stopStream();
    showScreen('Device disconnected', 'Plug it back in and it will reconnect automatically.', 'Try again');
  }

  // ---------- Plug and unplug ----------

  async function onDeviceChange() {
    if (!started) return;
    const before = knownVideoIds;
    try { await listDevices(); } catch { return; }
    rememberVideoIds();
    const added = videos.filter((d) => d.deviceId && !before.has(d.deviceId));
    const newCapture = added.find((d) => looksLikeCapture(d.label));

    if (!stream) {
      // Waiting for a device: connect to whatever was just plugged in.
      // Before access is granted the browser hides device ids, so a new
      // device shows up as an entry with no id; start() then asks for access.
      const hiddenIds = videos.length > 0 && !videos.some((d) => d.deviceId);
      // A quick unplug and replug (or a card that resets when the console
      // changes resolution) never looks "new", so also watch for the device
      // we just lost coming back.
      const lostIsBack = !!lostVideoId && videos.some((d) => d.deviceId === lostVideoId);
      if (added.length) start((newCapture || added[0]).deviceId);
      else if (lostIsBack) start(lostVideoId);
      else if (hiddenIds) start();
      return;
    }
    const current = videos.find((d) => d.deviceId === currentVideoId);
    if (!current) {
      onDisconnect();
      return;
    }
    // Take over from a webcam, but never interrupt a capture card mid-game.
    if (newCapture && !looksLikeCapture(current.label)) {
      toast(`Switched to ${newCapture.label}`);
      open(newCapture.deviceId, chooseAudio(newCapture));
      return;
    }
    // A capture card's sound device can register a moment after its picture.
    if (!currentAudioId) {
      const lateAudio = chooseAudio(current);
      if (lateAudio) {
        open(current.deviceId, lateAudio);
        return;
      }
    }
    renderSelects();
  }

  let deviceChangeTimer;
  function scheduleDeviceChange() {
    // One plug-in fires several events (video, then audio); wait for them to settle.
    clearTimeout(deviceChangeTimer);
    deviceChangeTimer = setTimeout(onDeviceChange, 500);
  }

  // ---------- Picture info, black-screen check ----------

  function updateStats() {
    const track = stream && stream.getVideoTracks()[0];
    if (!track) { stats.textContent = ''; return; }
    const set = track.getSettings();
    const w = video.videoWidth || set.width;
    const h = video.videoHeight || set.height;
    const fps = set.frameRate ? ` · ${Math.round(set.frameRate)} fps` : '';
    stats.textContent = w && h ? `${w}×${h}${fps}` : '';
  }

  const probeCanvas = document.createElement('canvas');
  probeCanvas.width = 32;
  probeCanvas.height = 18;
  const probeCtx = probeCanvas.getContext('2d', { willReadFrequently: true });
  let darkSamples = 0;
  let hintDismissed = false;

  function checkForBlackPicture() {
    if (!stream || video.readyState < 2 || !video.videoWidth) { darkSamples = 0; return; }
    let brightest = 0;
    try {
      probeCtx.drawImage(video, 0, 0, probeCanvas.width, probeCanvas.height);
      const px = probeCtx.getImageData(0, 0, probeCanvas.width, probeCanvas.height).data;
      for (let i = 0; i < px.length; i += 4) {
        const m = Math.max(px[i], px[i + 1], px[i + 2]);
        if (m > brightest) brightest = m;
      }
    } catch { return; }
    // A dark game scene still has some bright pixels; a blocked signal has none.
    // Blocked signals are often dark grey (level 16) with some noise, not pure 0.
    darkSamples = brightest < 24 ? darkSamples + 1 : 0;
    if (darkSamples === 0) hint.hidden = true;
    else if (darkSamples >= 7 && !hintDismissed) hint.hidden = false; // about 10 seconds
  }

  // ---------- Keep the screen awake ----------
  // Playing with a controller sends no keyboard or mouse input to the PC,
  // so without this Windows would dim or lock the screen mid-game.

  async function holdWake() {
    if (wakeLock || !('wakeLock' in navigator) || document.visibilityState !== 'visible') return;
    try {
      const lock = await navigator.wakeLock.request('screen');
      if (!stream) { lock.release().catch(() => {}); return; }
      wakeLock = lock;
      lock.addEventListener('release', () => { if (wakeLock === lock) wakeLock = null; });
    } catch { /* not allowed right now; retried when the tab is visible again */ }
  }

  function releaseWake() {
    if (wakeLock) { wakeLock.release().catch(() => {}); wakeLock = null; }
  }

  // ---------- Controls ----------

  function updateMuteButton() {
    const hasSound = !!(stream && stream.getAudioTracks().length);
    muteBtn.textContent = video.muted ? 'Unmute' : 'Mute';
    muteBtn.disabled = !hasSound;
    volume.disabled = !hasSound;
  }

  function toggleMute() {
    video.muted = !video.muted;
    updateMuteButton();
  }

  function toggleFill() {
    const fill = video.classList.toggle('fill');
    fillBtn.textContent = fill ? 'Fit' : 'Fill';
    store.set('fill', fill ? '1' : '0');
  }

  async function toggleFullscreen() {
    try {
      if (document.fullscreenElement) await document.exitFullscreen();
      else await document.documentElement.requestFullscreen();
    } catch { toast('Full screen is not available here.'); }
  }

  async function togglePip() {
    try {
      if (document.pictureInPictureElement) await document.exitPictureInPicture();
      else if (stream) await video.requestPictureInPicture();
    } catch { toast('The mini window is not available in this browser.'); }
  }

  function screenshot() {
    if (!stream || !video.videoWidth) return;
    const canvas = document.createElement('canvas');
    canvas.width = video.videoWidth;
    canvas.height = video.videoHeight;
    canvas.getContext('2d').drawImage(video, 0, 0);
    canvas.toBlob((blob) => {
      if (!blob) return;
      saveFile(blob, 'png');
      toast('Screenshot saved');
    }, 'image/png');
  }

  function saveFile(blob, extension) {
    const stamp = new Date().toISOString().slice(0, 19).replace(/[-:]/g, '').replace('T', '-');
    const link = document.createElement('a');
    link.href = URL.createObjectURL(blob);
    link.download = `console-viewer-${stamp}.${extension}`;
    link.click();
    setTimeout(() => URL.revokeObjectURL(link.href), 60000);
  }

  // ---------- Recording clips ----------
  // MP4 plays everywhere, so prefer it; older browsers fall back to WebM.

  const RECORDING_TYPES = [
    'video/mp4;codecs=avc1.42E01E,mp4a.40.2',
    'video/mp4',
    'video/webm;codecs=vp9,opus',
    'video/webm;codecs=vp8,opus',
    'video/webm',
  ];
  let recorder = null;
  let recordingSince = 0;
  let recordingTimer = null;

  function updateRecordingUi() {
    const on = !!recorder;
    const seconds = on ? Math.floor((Date.now() - recordingSince) / 1000) : 0;
    recTime.textContent = `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
    recBadge.hidden = !on;
    recBtn.textContent = on ? 'Stop recording' : 'Record';
    recBtn.classList.toggle('recording', on);
  }

  function startRecording() {
    if (!stream || recorder) return;
    const type = RECORDING_TYPES.find((t) => MediaRecorder.isTypeSupported(t)) || '';
    let rec;
    try {
      rec = new MediaRecorder(stream, { ...(type ? { mimeType: type } : {}), videoBitsPerSecond: 8000000 });
    } catch {
      toast('Could not start recording.');
      return;
    }
    const chunks = [];
    rec.ondataavailable = (e) => { if (e.data && e.data.size) chunks.push(e.data); };
    rec.onstop = () => {
      if (recorder === rec) stopRecording(); // the browser stopped it, not us
      const mime = rec.mimeType || type || 'video/webm';
      const blob = new Blob(chunks, { type: mime });
      if (!blob.size) return;
      saveFile(blob, mime.includes('mp4') ? 'mp4' : 'webm');
      toast('Clip saved');
    };
    rec.onerror = () => { toast('Recording stopped because of a problem.'); stopRecording(); };
    try {
      rec.start(1000);
    } catch {
      toast('Could not start recording.');
      return;
    }
    recorder = rec;
    recordingSince = Date.now();
    recordingTimer = setInterval(updateRecordingUi, 500);
    updateRecordingUi();
  }

  function stopRecording() {
    if (!recorder) return;
    const rec = recorder;
    recorder = null;
    clearInterval(recordingTimer);
    try { if (rec.state !== 'inactive') rec.stop(); } catch { /* already stopped */ }
    updateRecordingUi();
  }

  function toggleRecording() {
    if (recorder) stopRecording(); else startRecording();
  }

  // Hide the bar and the mouse pointer when the mouse has been still.
  let idleTimer;
  let menuTouchedAt = 0; // a menu left open only holds the bar for a few seconds
  function wake() {
    document.body.classList.remove('idle');
    clearTimeout(idleTimer);
    idleTimer = setTimeout(() => {
      const active = document.activeElement;
      const inMenu = !!active && active.tagName === 'SELECT';
      const busy = bar.matches(':hover') || (inMenu && Date.now() - menuTouchedAt < 8000);
      if (stream && !busy) {
        if (inMenu) active.blur(); // hand the keyboard shortcuts back
        document.body.classList.add('idle');
      } else {
        wake();
      }
    }, 2500);
  }

  // ---------- Wiring ----------

  function init() {
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
      const insecure = !window.isSecureContext;
      showScreen(
        insecure ? 'This page must be opened from localhost or https' : 'This browser cannot open capture devices',
        insecure ? 'Start a local server (see the README) and open http://localhost:8000.'
          : 'Try a current version of Chrome, Edge or Firefox.',
      );
      return;
    }

    const savedQuality = store.get('quality');
    if (savedQuality && QUALITY[savedQuality]) qualitySel.value = savedQuality;
    if (store.get('fill') === '1') { video.classList.add('fill'); fillBtn.textContent = 'Fit'; }
    const savedVolume = parseFloat(store.get('volume'));
    if (savedVolume >= 0 && savedVolume <= 1) { video.volume = savedVolume; volume.value = String(savedVolume); }
    if (!document.pictureInPictureEnabled) pipBtn.hidden = true;

    startBtn.addEventListener('click', () => start());

    videoSel.addEventListener('change', () => {
      const device = videos.find((d) => d.deviceId === videoSel.value);
      videoSel.blur();
      if (!device) return;
      store.set('video', device.deviceId);
      open(device.deviceId, chooseAudio(device));
    });
    audioSel.addEventListener('change', () => {
      const id = audioSel.value;
      audioSel.blur();
      if (!currentVideoId) return;
      store.set('audio:' + currentVideoId, id || 'none');
      open(currentVideoId, id);
    });
    qualitySel.addEventListener('change', () => {
      qualitySel.blur();
      store.set('quality', qualitySel.value);
      if (currentVideoId) open(currentVideoId, currentAudioId);
    });

    [videoSel, audioSel, qualitySel].forEach((select) => {
      ['focus', 'pointerdown', 'keydown'].forEach((type) => {
        select.addEventListener(type, () => { menuTouchedAt = Date.now(); });
      });
    });

    muteBtn.addEventListener('click', toggleMute);
    volume.addEventListener('input', () => {
      video.volume = Number(volume.value);
      if (video.muted && video.volume > 0) { video.muted = false; updateMuteButton(); }
      store.set('volume', volume.value);
    });
    fillBtn.addEventListener('click', toggleFill);
    shotBtn.addEventListener('click', screenshot);
    if (window.MediaRecorder) recBtn.addEventListener('click', toggleRecording);
    else recBtn.hidden = true;
    pipBtn.addEventListener('click', togglePip);
    fsBtn.addEventListener('click', toggleFullscreen);
    video.addEventListener('dblclick', toggleFullscreen);
    hintClose.addEventListener('click', () => { hintDismissed = true; hint.hidden = true; });

    video.addEventListener('resize', updateStats);
    video.addEventListener('loadedmetadata', updateStats);

    document.addEventListener('keydown', (e) => {
      if (e.ctrlKey || e.metaKey || e.altKey) return;
      const tag = e.target && e.target.tagName;
      if (tag === 'SELECT' || tag === 'INPUT') return;
      const actions = { f: toggleFullscreen, m: toggleMute, s: screenshot, p: togglePip, z: toggleFill };
      if (window.MediaRecorder) actions.r = toggleRecording;
      const action = actions[e.key.toLowerCase()];
      if (action && stream) { e.preventDefault(); action(); }
    });

    ['pointermove', 'pointerdown', 'keydown'].forEach((type) => document.addEventListener(type, wake));
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'visible' && stream) holdWake();
    });
    navigator.mediaDevices.addEventListener('devicechange', scheduleDeviceChange);
    setInterval(checkForBlackPicture, 1500);

    // If access was granted on an earlier visit, skip the Start button.
    if (navigator.permissions && navigator.permissions.query) {
      navigator.permissions.query({ name: 'camera' })
        .then((status) => { if (status.state === 'granted' && !started) start(); })
        .catch(() => { /* Firefox does not know 'camera'; the Start button still works */ });
    }
  }

  init();
})();
