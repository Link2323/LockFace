const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// The page file list is parsed from ENROLLMENT_JS_SOURCES in
// enrollment_app/CMakeLists.txt — the single source of truth. The files are
// evaluated one by one here in that build order (concatenation order IS the
// script's dependency order), so app_main.js (the composition root and the
// only file with executable top-level code) must stay last in the CMake
// list; it builds the app against these mocks and leaves it on the context
// as consoleApp. Renaming/removing a module breaks this parse loudly, so no
// parallel list to keep in sync.
const CMAKE_JS_LIST = fs.readFileSync(
  path.join(__dirname, '../../enrollment_app/CMakeLists.txt'), 'utf8'
).match(/set\(ENROLLMENT_JS_SOURCES\s*([^)]*)\)/);
assert.ok(CMAKE_JS_LIST, 'ENROLLMENT_JS_SOURCES not found in enrollment_app/CMakeLists.txt');
const PAGE_SOURCES = [...CMAKE_JS_LIST[1].matchAll(/js\/([\w.-]+\.js)/g)].map(m => m[1]);


function loadI18n(context) {
  context.LF_MESSAGES = Object.fromEntries(['zh-CN', 'en'].map(lang => [lang,
    JSON.parse(fs.readFileSync(path.join(__dirname, '../../enrollment_app/locales', lang + '.json'), 'utf8'))]));
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../../enrollment_app/js/i18n.js'), 'utf8'), context);
}

function fixture() {
  let id = 0;
  const pending = new Map();
  const timers = {
    setInterval(callback) { pending.set(++id, callback); return id; },
    setTimeout(callback) { pending.set(++id, callback); return id; },
    clearInterval(key) { pending.delete(key); },
    clearTimeout(key) { pending.delete(key); }
  };
  const context = vm.createContext({});
  vm.runInContext(fs.readFileSync(path.join(__dirname,
    '../../enrollment_app/js/capture_flow.js'), 'utf8'), context);
  return { flow: context.createCaptureFlow(timers), pending };
}

// preview.js paints frames on 2D contexts; the calls are no-ops here but the
// methods must exist or the synchronous onload chain throws before the
// cam-loading overlay logic runs.
function canvasContext() {
  const ctx = {};
  for (const m of ['setTransform', 'fillRect', 'save', 'restore', 'drawImage',
                   'clearRect', 'beginPath', 'moveTo', 'lineTo', 'arcTo',
                   'quadraticCurveTo', 'closePath', 'stroke', 'fill', 'arc',
                   'fillText']) ctx[m] = () => {};
  ctx.measureText = () => ({ width: 10 });
  return ctx;
}

// Full-page harness: mock timers/DOM/host, evaluate every module file in
// build order, and expose the pieces tests need to poke at.
function page(overrides) {
  overrides = overrides || {};
  let id = 0;
  const intervals = new Map(), timeouts = new Map(), elements = new Map();
  const timers = {
    setInterval(fn) { intervals.set(++id, fn); return id; },
    clearInterval(key) { intervals.delete(key); },
    setTimeout(fn) { timeouts.set(++id, fn); return id; },
    clearTimeout(key) { timeouts.delete(key); }
  };
  function element(key) {
    if (!elements.has(key)) elements.set(key, {
      checked: true, style: {}, disabled: false, value: '', textContent: '',
      children: [], attrs: {},
      classList: (() => {
        const set = new Set();
        return {
          add(c) { set.add(c); },
          remove(c) { set.delete(c); },
          toggle(c, on) { (on === undefined ? !set.has(c) : on) ? set.add(c) : set.delete(c); },
          contains(c) { return set.has(c); }
        };
      })(),
      setAttribute(n, v) { this.attrs[n] = String(v); },
      getAttribute(n) { return n in this.attrs ? this.attrs[n] : null; },
      removeAttribute(n) { delete this.attrs[n]; },
      contains() { return false; },
      addEventListener() {}, focus() {}, select() {}, querySelector: () => null, getContext: () => canvasContext()
    });
    return elements.get(key);
  }
  const state = {
    status: { done: true, livenessPassed: true, collected: 5, target: 5 },
    config: { camera_rotation: 90, ema_learning: true, secret_key: 'keep-me',
              match_threshold: 0.85, anti_spoof_threshold: 0.28,
              learning_distance_gate: 0.65 }
  };
  const calls = { facesJson: 0, setConfig: [] };
  const host = Object.assign({
    GetUsername: () => 'test', GetFaceCount: () => 1,
    CaptureFaceSamples() { return true; },
    GetCaptureStatus: () => JSON.stringify(state.status),
    CancelCapture() {},
    GetFacesJson() { calls.facesJson++; return JSON.stringify([{ id: 1, label: 'test-face' }]); },
    GetConfig: () => JSON.stringify(state.config),
    BeginConfigSave(json) { calls.setConfig.push(json); return true; },
    GetConfigSaveStatus: () => 'saved'
  }, overrides.host);
  const context = vm.createContext({
    ...timers, window: { ...timers, chrome: { webview: { hostObjects: { sync: { host } } } } },
    document: { getElementById: element, documentElement: element('html'),
                querySelectorAll: () => [], addEventListener() {} },
    // preview.js delivers frames to the canvas by assigning img.src and
    // waiting for onload; fire it synchronously so frame arrival — and the
    // cam-loading overlay handoff tied to it — is observable in tests.
    Image: function() {
      this.naturalWidth = 640; this.naturalHeight = 480; this.complete = true;
      let src = '';
      Object.defineProperty(this, 'src', {
        get() { return src; },
        set(v) { src = v; if (typeof this.onload === 'function') this.onload(); }
      });
    },
    console
  });
  loadI18n(context);
  for (const file of PAGE_SOURCES) {
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../../enrollment_app/js', file), 'utf8'), context);
  }
  // The real page opens on the consent gate; most tests exercise the
  // post-consent UI. grant() marks it accepted without running the startup
  // callback (which would start the preview frame loop in this mock).
  if (overrides.grantConsent !== false) context.consoleApp.consent.grant();
  return { context, app: context.consoleApp, intervals, timeouts, elements, element, calls, state };
}

// Drive real preview polling with observable canvas writes and asynchronous
// image loads; this catches redundant rendering without requiring a camera.
function previewFixture(initialFrame = 'jpeg-a') {
  const counts = { sizeWrites: 0, images: 0, overlays: 0 };
  const pending = new Map();
  let timer = 0, image, imageRequests = 0;
  const bgContext = canvasContext(), faceContext = canvasContext();
  bgContext.drawImage = () => counts.images++;
  faceContext.clearRect = () => counts.overlays++;
  const bg = { getContext: () => bgContext };
  for (const [key, initial] of [['width', 300], ['height', 150]]) {
    let value = initial;
    Object.defineProperty(bg, key, {
      get: () => value,
      set(v) { value = v; counts.sizeWrites++; }
    });
  }
  const viewport = { clientWidth: 640, clientHeight: 360 };
  const loading = { classList: { add() {} }, style: {} };
  const nodes = { 'bg-canvas': bg, viewport, 'cam-loading': loading,
    'face-canvas': { getContext: () => faceContext } };
  const state = { frame: initialFrame, faces: '[]' };
  const transport = { polls: 0, payloads: 0 };
  let version = 0, previousSnapshot = '';
  const context = vm.createContext({
    window: { devicePixelRatio: 1 },
    document: { getElementById: key => nodes[key] },
    setTimeout(fn) { pending.set(++timer, fn); return timer; },
    clearTimeout(key) { pending.delete(key); },
    Image: function() {
      image = this;
      this.naturalWidth = 1280; this.naturalHeight = 720; this.complete = false;
      Object.defineProperty(this, 'src', { set() { this.complete = false; imageRequests++; } });
    }
  });
  loadI18n(context);
  vm.runInContext(fs.readFileSync(path.join(__dirname,
    '../../enrollment_app/js/preview.js'), 'utf8'), context);
  const preview = context.createPreview({ host: {
    StartPreview: () => true, SetPreviewSize() {},
    GetLatestFrameAndFaces(knownVersion) {
      transport.polls++;
      const snapshot = state.frame + '\x1e' + state.faces;
      if (snapshot !== previousSnapshot) { version++; previousSnapshot = snapshot; }
      if (knownVersion === String(version)) return '';
      transport.payloads++;
      return String(version) + '\x1e' + snapshot;
    }
  }, showBanner() {}, beginPreviewStatus: () => ({ ready() {}, failed() {} }) });
  preview.setConsentGranted(true);
  preview.restartPreview();
  return { counts, viewport, state, context, preview, loading, transport,
    imageRequests: () => imageRequests,
    load() { image.complete = true; image.onload(); },
    imageCompletion: () => image.onload,
    callbacks: () => [...pending.values()],
    flush() {
      const queued = [...pending.values()];
      pending.clear();
      for (const fn of queued) fn();
    },
    poll() {
      const entry = [...pending].find(([, fn]) => fn.name === 'renderFrameTick');
      assert.ok(entry, 'preview polling is scheduled');
      pending.delete(entry[0]);
      entry[1]();
    }
  };
}

test('an empty backend cache waits for a valid JPEG without decoding the separator', () => {
  const p = previewFixture('');
  p.poll();
  assert.equal(p.imageRequests(), 0);
  assert.equal(p.counts.images, 0);
  p.state.frame = 'jpeg-a';
  p.poll(); p.load();
  assert.equal(p.imageRequests(), 1);
  assert.equal(p.counts.images, 1);
});

test('unchanged preview polls do not resize or repaint either canvas', () => {
  const p = previewFixture();
  p.load();
  assert.deepEqual(p.counts, { sizeWrites: 2, images: 1, overlays: 1 });
  for (let i = 0; i < 30; i++) p.poll();
  assert.deepEqual(p.counts, { sizeWrites: 2, images: 1, overlays: 1 });
  assert.deepEqual(p.transport, { polls: 31, payloads: 1 });
  p.state.frame = 'jpeg-b';
  p.poll(); p.load();
  assert.deepEqual(p.counts, { sizeWrites: 2, images: 2, overlays: 2 });
  assert.equal(p.transport.payloads, 2);
});

test('overlay changes repaint only the overlay and wait for pending JPEG loads', () => {
  const p = previewFixture();
  p.load();
  p.state.faces = '[ ]';
  p.poll();
  assert.deepEqual(p.counts, { sizeWrites: 2, images: 1, overlays: 2 });
  p.state.frame = 'jpeg-b'; p.state.faces = '[]';
  p.poll(); p.poll();
  assert.deepEqual(p.counts, { sizeWrites: 2, images: 1, overlays: 2 });
  p.load();
  assert.deepEqual(p.counts, { sizeWrites: 2, images: 2, overlays: 3 });
});

test('layout and DPI changes redraw a stationary preview at physical resolution', () => {
  const p = previewFixture();
  p.load();
  p.viewport.clientWidth = 800; p.viewport.clientHeight = 450;
  p.poll();
  assert.deepEqual(p.counts, { sizeWrites: 4, images: 2, overlays: 2 });
  p.context.window.devicePixelRatio = 1.5;
  p.poll();
  assert.deepEqual(p.counts, { sizeWrites: 6, images: 3, overlays: 3 });
  p.poll();
  assert.equal(p.counts.images, 3);
});

test('restarting with an identical JPEG dismisses the loading overlay again', () => {
  const p = previewFixture();
  p.load();
  p.preview.stopLoop();
  p.loading.classList.add = () => { p.loading.dismissed = true; };
  p.preview.restartPreview();
  p.load();
  assert.equal(p.loading.dismissed, true);
  assert.equal(p.counts.sizeWrites, 2);
  assert.equal(p.transport.payloads, 2, 'new viewing session forces a full snapshot');
});

test('caption buttons dispatch minimize and close to the native host', () => {
  const calls = [];
  const p = page({ host: {
    MinimizeWindow() { calls.push('minimize'); },
    CloseWindow() { calls.push('close'); }
  }});
  p.context.minimizeWindow();
  p.context.closeWindow();
  assert.deepEqual(calls, ['minimize', 'close']);
});

test('cancelling between angles clears timers and rejects already queued work after retry', () => {
  const { flow, pending } = fixture();
  let oldPolls = 0, oldAngles = 0, newPolls = 0;
  flow.begin(true, 0);
  flow.poll(() => oldPolls++);
  const oldPoll = [...pending.values()][0];
  flow.stopPolling();
  flow.scheduleNext(() => oldAngles++);
  const oldAngle = [...pending.values()][0];
  assert.equal(flow.isActive(), true); // the inter-angle gap is still capture
  flow.end();
  assert.equal(pending.size, 0);
  flow.begin(false, 2);
  flow.poll(() => newPolls++);
  oldPoll();
  oldAngle();
  [...pending.values()][0]();
  assert.deepEqual([oldPolls, oldAngles, newPolls], [0, 0, 1]);
  assert.equal(flow.firstAngle(), 2);
  assert.equal(flow.isMultiAngle(), false);
});

test('normal three-angle sequence stays active until confirmation', () => {
  const { flow, pending } = fixture();
  const angles = [];
  flow.begin(true, 0);
  function capture(angle) {
    angles.push(angle);
    if (angle < 2) flow.scheduleNext(() => capture(angle + 1));
    else flow.end();
  }
  capture(0);
  while (pending.size) {
    const [key, callback] = [...pending][0];
    pending.delete(key);
    callback();
  }
  assert.deepEqual(angles, [0, 1, 2]);
  assert.equal(flow.isActive(), false);
  assert.equal(flow.isMultiAngle(), true); // retained for the append screen
  assert.equal(flow.canShowIdleTotal(), false);
});

test('failure or completion prevents stale totals and poll callbacks', () => {
  const { flow, pending } = fixture();
  assert.equal(flow.canShowIdleTotal(), true);
  flow.begin(false, 1);
  assert.equal(flow.canShowIdleTotal(), false);
  let paints = 0;
  flow.poll(() => paints++);
  const queued = [...pending.values()][0];
  flow.end();
  flow.end(); // teardown is idempotent
  queued();
  assert.equal(paints, 0);
  assert.equal(pending.size, 0);
  assert.equal(flow.canShowIdleTotal(), false);
  assert.equal(flow.firstAngle(), 1);
});

test('capture is blocked until the per-user consent is accepted', () => {
  const p = page({ grantConsent: false });
  p.context.onCapture();
  assert.equal(p.app.captureCtl.isActive(), false);
  assert.ok(p.element('consent-overlay').classList.contains('active'));
});

test('a remembered record for the current notice version skips the gate', () => {
  const p = page({ grantConsent: false, host: {
    GetPrivacyConsent: () => JSON.stringify(
      { noticeVersion: '1.0', record: { version: '1.0', time: '2026-01-01T10:00:00+08:00' } })
  } });
  assert.equal(p.app.consent.granted(), true);   // silently resumed
  assert.ok(!p.element('consent-overlay').classList.contains('active'));
  p.context.onCapture();                          // camera path is open
  assert.equal(p.app.captureCtl.isActive(), true);
});

test('an outdated consent record keeps the gate up', () => {
  const p = page({ grantConsent: false, host: {
    GetPrivacyConsent: () => JSON.stringify(
      { noticeVersion: '1.1', record: { version: '1.0', time: '2026-01-01T10:00:00+08:00' } })
  } });
  assert.ok(p.element('consent-overlay').classList.contains('active'));
  assert.equal(p.app.consent.granted(), false);
});

test('accepting the gate persists the record through the host', () => {
  const wrote = [];
  const p = page({ grantConsent: false, host: {
    GetPrivacyConsent: () => JSON.stringify({ noticeVersion: '1.0', record: null }),
    AcceptPrivacyConsent() { wrote.push(1); return true; }
  } });
  p.element('consent-agree').checked = true;
  p.context.onConsentAccept();
  assert.deepEqual(wrote, [1]);
  assert.equal(p.app.consent.granted(), true);
  assert.ok(!p.element('consent-overlay').classList.contains('active'));
});

test('withdraw revokes the session consent and clears the host record', () => {
  const cleared = [];
  const rec = { noticeVersion: '1.0', record: { version: '1.0', time: '2026-01-02T03:04:05+08:00' } };
  const p = page({ host: {
    GetPrivacyConsent: () => JSON.stringify(rec),
    WithdrawPrivacyConsent() { rec.record = null; cleared.push(1); return true; }
  } });
  assert.equal(p.element('btn-consent-status').textContent, '已同意隐私说明');  // pill reflects record
  p.app.consent.withdraw();
  assert.deepEqual(cleared, [1]);
  assert.equal(p.app.consent.granted(), false);
  assert.equal(p.app.previewCtl.consentGranted(), false);
  assert.equal(p.element('btn-consent-status').textContent, '未同意隐私说明');
});

for (const failure of ['refused', 'disconnected']) {
  test('withdrawal failure remains retryable: ' + failure, () => {
    let stops = 0, deletes = 0, lateCaptures = 0;
    const rec = { noticeVersion: '1.0', record: { version: '1.0', time: '2026-01-02T03:04:05+08:00' } };
    const p = page({ host: {
      GetPrivacyConsent: () => JSON.stringify(rec),
      WithdrawPrivacyConsent() {
        deletes++;
        if (failure === 'disconnected') throw new Error('service disconnected');
        return false;
      },
      StopPreview() { stops++; }
    } });
    p.app.captureCtl.begin(true, 0);
    p.app.captureCtl.scheduleNext(() => lateCaptures++);
    const queued = [...p.timeouts.values()];
    p.element('fld-pass').value = 'unsaved-password';
    p.app.consent.withdraw();
    for (const fn of queued) fn();
    assert.equal(deletes, 1);
    assert.ok(stops >= 1);
    assert.equal(p.app.consent.granted(), false);
    assert.equal(p.app.captureCtl.isActive(), false);
    assert.equal(lateCaptures, 0);
    assert.equal(p.element('fld-pass').value, '');
    assert.ok(rec.record, 'retain consent record until deletion is confirmed');
    assert.match(p.element('cam-banner').textContent, /未能确认/);
    p.context.onConsentStatus();
    assert.equal(p.element('modal-mid').textContent, '重试撤回');
    p.context.onModalMid();
    assert.equal(deletes, 2);
  });
}

for (const hasRecord of [true, false]) {
  test('reopened pending withdrawal blocks auto-resume and reconsent, record=' + hasRecord, () => {
    let starts = 0, accepts = 0, retries = 0;
    const rec = { noticeVersion: '1.0', withdrawalPending: true,
      record: hasRecord ? { version: '1.0', time: '2026-01-02T03:04:05+08:00' } : null };
    const host = {
      GetPrivacyConsent: () => JSON.stringify(rec),
      AcceptPrivacyConsent() { accepts++; return true; },
      StartPreview() { starts++; return true; },
      StopPreview() {},
      WithdrawPrivacyConsent() { retries++; return false; }
    };
    const first = page({ grantConsent: false, host });
    // A new page has no in-memory failure flag: only the persisted host state
    // can stop startup from trusting the old consent record.
    const p = page({ grantConsent: false, host });
    for (const fn of [...p.timeouts.values()]) fn();
    assert.equal(first.app.consent.granted(), false);
    assert.equal(p.app.consent.granted(), false);
    assert.equal(starts, 0);
    assert.equal(p.element('btn-consent-status').textContent, '撤回处理中');
    assert.equal(p.element('consent-overlay').classList.contains('active'), false);
    p.element('consent-agree').checked = true;
    p.context.onConsentAccept();
    p.context.onCapture();
    assert.equal(accepts, 0);
    assert.equal(p.app.captureCtl.isActive(), false);
    assert.equal(p.element('modal-mid').textContent, '重试撤回');
    p.context.onModalMid();
    assert.equal(retries, 1);
    assert.equal(p.app.consent.granted(), false);
    p.app.host.WithdrawPrivacyConsent = () => {
      rec.withdrawalPending = false;
      rec.record = null;
      return true;
    };
    p.app.consent.withdraw();
    assert.equal(p.element('btn-consent-status').textContent, '未同意隐私说明');
    p.context.onConsentStatus();
    assert.equal(p.element('modal-mid').textContent, '重新同意');
    assert.equal(starts, 0, 'completed withdrawal never resumes the old preview callback');
    p.context.onModalMid();
    p.element('consent-agree').checked = true;
    p.context.onConsentAccept();
    assert.equal(accepts, 1);
    assert.equal(p.app.consent.granted(), true);
  });
}

for (const failure of ['write-refused', 'host-error']) {
  test('failed acceptance never grants preview: ' + failure, () => {
    let starts = 0;
    const p = page({ grantConsent: false, host: {
      GetPrivacyConsent: () => JSON.stringify({ noticeVersion: '1.0', record: null }),
      AcceptPrivacyConsent() {
        if (failure === 'host-error') throw new Error('registry unavailable');
        return false;
      },
      StartPreview() { starts++; return true; }
    } });
    p.element('consent-agree').checked = true;
    p.context.onConsentAccept();
    for (const fn of [...p.timeouts.values()]) fn();
    assert.equal(p.app.consent.granted(), false);
    assert.equal(starts, 0);
    assert.match(p.element('cam-banner').textContent, /未能保存同意/);
  });
}

test('immediate capture and reconsent clears the completed withdrawal banner', () => {
  const rec = { noticeVersion: '1.1', record: { version: '1.1' } };
  const p = page({ host: {
    GetPrivacyConsent: () => JSON.stringify(rec),
    WithdrawPrivacyConsent() { rec.record = null; return true; },
    AcceptPrivacyConsent() { rec.record = { version: '1.1' }; return true; },
    StartPreview: () => true, StopPreview() {}, GetFaceCount: () => 0
  } });
  p.app.consent.withdraw();
  assert.match(p.element('cam-banner').textContent, /已撤回同意/);
  p.context.onCapture();
  p.element('consent-agree').checked = true;
  p.context.onConsentAccept();
  assert.equal(p.app.captureCtl.isActive(), true);
  assert.equal(p.app.consent.granted(), true);
  assert.equal(p.element('cam-banner').className, 'banner', 'old withdrawal message must not remain during new capture');
});

test('reconsent preserves a new preview failure against the old withdrawal timeout', () => {
  const rec = { noticeVersion: '1.1', record: { version: '1.1' } };
  const p = page({ host: {
    GetPrivacyConsent: () => JSON.stringify(rec),
    WithdrawPrivacyConsent() { rec.record = null; return true; },
    AcceptPrivacyConsent() { rec.record = { version: '1.1' }; return true; },
    StartPreview: () => false, StopPreview() {}
  } });
  p.timeouts.clear();
  p.app.consent.withdraw();
  const oldTimeouts = [...p.timeouts.values()];
  p.context.onCapture();
  p.element('consent-agree').checked = true;
  p.context.onConsentAccept();
  for (const fn of oldTimeouts) fn();
  assert.equal(p.element('cam-banner').className, 'banner error');
  assert.match(p.element('cam-banner').textContent, /预览启动失败/);
  assert.equal(p.app.captureCtl.isActive(), false);
});

test('a late image completion after stopping preview cannot hide the blocked overlay', () => {
  const p = previewFixture();
  p.preview.stopLoop();
  p.loading.style.display = 'flex';
  p.load();
  p.flush();
  assert.equal(p.loading.style.display, 'flex');
  assert.equal(p.counts.images, 0, 'revoked preview must not paint a late image');
});

test('old preview polling cannot create an extra loop after restart', () => {
  const p = previewFixture();
  const queued = p.callbacks();
  p.preview.restartPreview();
  const polls = p.transport.polls;
  for (const fn of queued) fn();
  assert.equal(p.transport.polls, polls, 'old poll must not access the host in the new session');
});

test('old preview image and overlay callbacks cannot finish a new viewing session', () => {
  const p = previewFixture();
  const oldImageLoaded = p.imageCompletion();
  p.load();
  const oldTimers = p.callbacks();
  p.preview.stopLoop();
  p.loading.style.display = 'flex';
  p.preview.restartPreview();
  const images = p.counts.images;
  oldImageLoaded();
  for (const fn of oldTimers) fn();
  assert.equal(p.counts.images, images, 'old image must not repaint after a restart');
  assert.equal(p.loading.style.display, 'flex', 'old hide timer must not dismiss the new loading overlay');
  p.load();
  p.flush();
  assert.equal(p.loading.style.display, 'none', 'the new frame still completes the new session');
});

test('startup callbacks from the withdrawn consent cannot restart a newly granted capture', () => {
  let starts = 0;
  const rec = { noticeVersion: '1.1', record: { version: '1.1' } };
  const p = page({ grantConsent: false, host: {
    GetPrivacyConsent: () => JSON.stringify(rec),
    WithdrawPrivacyConsent() { rec.record = null; return true; },
    AcceptPrivacyConsent() { rec.record = { version: '1.1' }; return true; },
    StartPreview() { starts++; return true; }, StopPreview() {}, GetFaceCount: () => 0
  } });
  const queuedStartup = [...p.timeouts.values()];
  p.app.consent.withdraw();
  p.context.onCapture();
  p.element('consent-agree').checked = true;
  p.context.onConsentAccept();
  assert.equal(starts, 1);
  for (const fn of queuedStartup) fn();
  assert.equal(starts, 1, 'old startup work must not restart the new preview session');
});

for (const action of ['onSave', 'onAppendConfirm']) {
  test('queued enrollment write is discarded across withdrawal and reconsent: ' + action, () => {
    let validations = 0, writes = 0;
    const rec = { noticeVersion: '1.1', record: { version: '1.1' } };
    const p = page({ host: {
      GetPrivacyConsent: () => JSON.stringify(rec),
      WithdrawPrivacyConsent() { rec.record = null; return true; },
      AcceptPrivacyConsent() { rec.record = { version: '1.1' }; return true; },
      StartPreview: () => true, StopPreview() {},
      ValidatePassword() { validations++; return true; },
      SaveEnrollment() { writes++; return true; },
      SaveEnrollmentAppend() { writes++; return true; }
    } });
    p.timeouts.clear();
    p.element('fld-pass').value = 'synthetic-test-password';
    p.app.enrollSave[action]();
    const queuedWrites = [...p.timeouts.values()];
    p.app.consent.withdraw();
    p.context.onConsentStatus();
    p.context.onModalMid();
    p.element('consent-agree').checked = true;
    p.context.onConsentAccept();
    for (const fn of queuedWrites) fn();
    assert.equal(validations, 0);
    assert.equal(writes, 0);
    assert.equal(p.element(action === 'onSave' ? 'btn-save' : 'btn-append').disabled, false);
  });
}

test('a previous camera save cannot stop or restart capture after reconsent', () => {
  let status = 'pending', starts = 0, stops = 0;
  const rec = { noticeVersion: '1.1', record: { version: '1.1' } };
  const p = page({ host: {
    GetPrivacyConsent: () => JSON.stringify(rec),
    WithdrawPrivacyConsent() { rec.record = null; return true; },
    AcceptPrivacyConsent() { rec.record = { version: '1.1' }; return true; },
    StartPreview() { starts++; return true; }, StopPreview() { stops++; },
    BeginConfigSave: () => true, GetConfigSaveStatus: () => status, GetFaceCount: () => 0
  } });
  p.timeouts.clear();
  p.element('cam-picker-select').value = 'test-camera';
  p.app.settings.onCamPickerConfirm();
  runDeferred(p);
  p.app.consent.withdraw();
  p.context.onCapture();
  p.element('consent-agree').checked = true;
  p.context.onConsentAccept();
  const before = { starts, stops };
  status = 'saved';
  runDeferred(p);
  runDeferred(p);
  assert.deepEqual({ starts, stops }, before, 'old camera result must not change the new camera session');
  assert.equal(p.app.captureCtl.isActive(), true);
  assert.equal(p.element('cam-banner').className, 'banner');
  assert.equal(p.element('cam-picker-confirm').disabled, false, 'old settings operation must still release its controls');
});

test('a queued camera change is cancelled before reaching the native host after withdrawal', () => {
  let writes = 0;
  const rec = { noticeVersion: '1.1', record: { version: '1.1' } };
  const p = page({ host: {
    GetPrivacyConsent: () => JSON.stringify(rec),
    WithdrawPrivacyConsent() { rec.record = null; return true; },
    AcceptPrivacyConsent() { rec.record = { version: '1.1' }; return true; },
    StartPreview: () => true, StopPreview() {}, GetFaceCount: () => 0,
    BeginConfigSave() { writes++; return true; }
  } });
  p.timeouts.clear();
  p.element('cam-picker-select').value = 'test-camera';
  p.app.settings.onCamPickerConfirm();
  const queued = [...p.timeouts.values()];
  p.app.consent.withdraw();
  p.context.onCapture();
  p.element('consent-agree').checked = true;
  p.context.onConsentAccept();
  for (const fn of queued) fn();
  assert.equal(writes, 0, 'native config save would restart the camera before the JS completion guard');
  assert.equal(p.element('cam-picker-confirm').disabled, false);
  assert.equal(p.app.captureCtl.isActive(), true);
});

for (const action of ['withdraw', 'decline']) {
  for (const outcome of ['frame-later', 'frame-immediate', 'start-failed']) {
    test('status-button reconsent updates the caption: ' + action + ' / ' + outcome, () => {
      let frameReady = outcome === 'frame-immediate';
      const rec = { noticeVersion: '1.1', record: action === 'withdraw' ? { version: '1.1' } : null };
      const p = page({ grantConsent: false, host: {
        GetPrivacyConsent: () => JSON.stringify(rec),
        WithdrawPrivacyConsent() { rec.record = null; return true; },
        AcceptPrivacyConsent() { rec.record = { version: '1.1' }; return true; },
        StartPreview: () => outcome !== 'start-failed', StopPreview() {},
        GetLatestFrameAndFaces: () => frameReady ? '1\x1edata:image/jpeg;base64,FRAME\x1e[]' : ''
      } });
      p.timeouts.clear();
      p.app.consent[action]();
      assert.equal(p.element('cam-caption').textContent, '未同意隐私说明，摄像头已停止。');
      p.context.onConsentStatus();
      p.context.onModalMid();
      p.element('consent-agree').checked = true;
      p.context.onConsentAccept();
      assert.equal(p.app.consent.granted(), true);
      assert.equal(rec.record.version, '1.1');
      assert.equal(p.app.captureCtl.isActive(), false, 'status reconsent resumes only preview');
      if (outcome === 'start-failed') {
        assert.match(p.element('cam-caption').textContent, /预览启动失败/);
        return;
      }
      if (!frameReady) {
        assert.match(p.element('cam-caption').textContent, /正在启动摄像头/);
        frameReady = true;
        const tick = [...p.timeouts.values()].find(fn => fn.name === 'renderFrameTick');
        assert.ok(tick);
        tick();
      }
      assert.match(p.element('cam-caption').textContent, /准备就绪/);
      assert.doesNotMatch(p.element('cam-caption').textContent, /需要同意|撤回处理中/);
    });
  }
}

test('first preview frame must preserve newer capture instructions after reconsent', () => {
  let frameReady = false;
  const rec = { noticeVersion: '1.1', record: { version: '1.1' } };
  const p = page({ host: {
    GetPrivacyConsent: () => JSON.stringify(rec),
    WithdrawPrivacyConsent() { rec.record = null; return true; },
    AcceptPrivacyConsent() { rec.record = { version: '1.1' }; return true; },
    StartPreview: () => true, StopPreview() {}, GetFaceCount: () => 0,
    GetLatestFrameAndFaces: () => frameReady ? '1\x1edata:image/jpeg;base64,FRAME\x1e[]' : ''
  } });
  p.app.consent.withdraw();
  p.context.onCapture();
  p.element('consent-agree').checked = true;
  p.context.onConsentAccept();
  assert.equal(p.app.captureCtl.isActive(), true);
  const instruction = p.element('cam-caption').textContent;
  assert.match(instruction, /请正对摄像头/);
  frameReady = true;
  [...p.timeouts.values()].find(fn => fn.name === 'renderFrameTick')();
  assert.equal(p.element('cam-caption').textContent, instruction);
});

for (const remembered of [false, true]) {
  test('startup caption follows first-frame readiness and survives idle polling: ' + remembered, () => {
    let frameReady = false;
    const rec = { noticeVersion: '1.1', record: remembered ? { version: '1.1' } : null };
    const p = page({ grantConsent: false, host: {
      GetPrivacyConsent: () => JSON.stringify(rec),
      AcceptPrivacyConsent() { rec.record = { version: '1.1' }; return true; },
      StartPreview: () => true,
      GetCaptureStatus: () => JSON.stringify({ done: true, total: 0 }),
      GetLatestFrameAndFaces: () => frameReady ? '1\x1edata:image/jpeg;base64,FRAME\x1e[]' : ''
    } });
    if (!remembered) {
      p.element('consent-agree').checked = true;
      p.context.onConsentAccept();
    }
    assert.match(p.element('cam-caption').textContent, /正在启动摄像头/);
    runDeferred(p);
    for (const fn of [...p.intervals.values()]) fn();
    assert.equal(p.app.previewCtl.isReady(), false);
    assert.match(p.element('cam-caption').textContent, /正在启动摄像头/);
    frameReady = true;
    [...p.timeouts.values()].find(fn => fn.name === 'renderFrameTick')();
    for (const fn of [...p.intervals.values()]) fn();
    assert.equal(p.app.previewCtl.isReady(), true);
    assert.match(p.element('cam-caption').textContent, /准备就绪/);
  });
}

for (const outcome of ['cancelled', 'failed']) {
  test('late first frame preserves the terminal capture caption: ' + outcome, () => {
    let frameReady = false;
    const p = page({ host: {
      StartPreview: () => true, GetFaceCount: () => 0,
      GetLatestFrameAndFaces: () => frameReady ? '1\x1edata:image/jpeg;base64,FRAME\x1e[]' : ''
    } });
    p.app.previewCtl.restartPreview();
    p.context.onCapture();
    if (outcome === 'cancelled') p.context.onCapture();
    else {
      p.state.status = { done: true, livenessPassed: false, failureReason: 'no_face' };
      [...p.intervals.values()].at(-1)();
    }
    const caption = p.element('cam-caption').textContent;
    assert.match(caption, outcome === 'cancelled' ? /已采 0 帧/ : /采集失败/);
    frameReady = true;
    [...p.timeouts.values()].find(fn => fn.name === 'renderFrameTick')();
    assert.equal(p.element('cam-caption').textContent, caption);
  });
}

test('returning to preview replaces an old camera failure caption', () => {
  let available = false;
  const p = page({ host: {
    StartPreview: () => available, StopPreview() {},
    GetLatestFrameAndFaces: () => '1\x1edata:image/jpeg;base64,FRAME\x1e[]'
  } });
  p.app.previewCtl.restartPreview();
  assert.match(p.element('cam-caption').textContent, /预览启动失败/);
  p.app.nav.switchTab('logs');
  available = true;
  p.app.nav.switchTab('cam');
  assert.match(p.element('cam-caption').textContent, /准备就绪/);
});

test('declining consent and returning to camera share one persistent state without duplicate banners', () => {
  const p = page({ grantConsent: false, host: {
    GetPrivacyConsent: () => JSON.stringify({ noticeVersion: '1.1', record: null }),
    StopPreview() {}
  } });
  p.app.consent.decline();
  const caption = p.element('cam-caption').textContent;
  assert.equal(caption, '未同意隐私说明，摄像头已停止。');
  assert.equal(p.element('cam-banner').className, 'banner');
  p.app.nav.switchTab('logs');
  p.app.nav.switchTab('cam');
  assert.equal(p.element('cam-caption').textContent, caption);
  assert.equal(p.element('cam-banner').className, 'banner');
});

test('returning to the camera does not replace a confirmed withdrawal result with a generic consent warning', () => {
  const rec = { noticeVersion: '1.1', record: { version: '1.1' } };
  const p = page({ host: {
    GetPrivacyConsent: () => JSON.stringify(rec),
    WithdrawPrivacyConsent() { rec.record = null; return true; },
    StopPreview() {}
  } });
  p.app.consent.withdraw();
  const result = p.element('cam-banner').textContent;
  assert.match(result, /已撤回同意/);
  p.app.nav.switchTab('logs');
  p.app.nav.switchTab('cam');
  assert.equal(p.element('cam-banner').textContent, result);
  assert.equal(p.element('cam-caption').textContent, '未同意隐私说明，摄像头已停止。');
});

test('the consent pill opens a status dialog with view and withdraw actions', () => {
  const rec = { noticeVersion: '1.0', record: { version: '1.0', time: '2026-01-02T03:04:05+08:00' } };
  const p = page({ host: {
    GetPrivacyConsent: () => JSON.stringify(rec)
  } });
  p.context.onConsentStatus();
  assert.ok(p.element('modal-overlay').classList.contains('active'));
  assert.equal(p.element('modal-ok').textContent, '查看隐私说明');
  assert.equal(p.element('modal-mid').textContent, '撤回同意');
  assert.ok(p.element('modal-message').textContent.includes('你已同意'));
  p.context.onModalCancel();
  assert.ok(!p.element('modal-overlay').classList.contains('active'));
});

test('an outdated notice can be withdrawn without accepting the new version', () => {
  let withdrawn = 0;
  const rec = { noticeVersion: '1.1', record: { version: '1.0', time: '2026-01-02T03:04:05+08:00' } };
  const p = page({ grantConsent: false, host: {
    GetPrivacyConsent: () => JSON.stringify(rec),
    WithdrawPrivacyConsent() { withdrawn++; rec.record = null; return true; }
  } });
  p.context.onConsentStatus();
  assert.equal(p.element('modal-mid').textContent, '撤回同意');
  p.context.onModalMid();
  assert.equal(withdrawn, 1);
  assert.equal(p.app.consent.granted(), false);
});

test('the status dialog offers consent without a current record', () => {
  const p = page({ host: {
    GetPrivacyConsent: () => JSON.stringify({ noticeVersion: '1.0', record: null })
  } });
  p.app.consent.statusMenu();
  assert.equal(p.element('modal-message').textContent,
               '未同意隐私说明，摄像头已停止。\n\n点击“重新同意”可恢复预览。');
  assert.equal(p.element('modal-mid').textContent, '重新同意');
  assert.notEqual(p.element('modal-mid').style.display, 'none');
});

for (const action of ['decline', 'withdraw']) {
  test(`${action} allows consent to be reopened and preview resumed from the status button`, () => {
    let starts = 0, accepts = 0;
    const rec = { noticeVersion: '1.0', record: null };
    const p = page({ grantConsent: false, host: {
      GetPrivacyConsent: () => JSON.stringify(rec),
      AcceptPrivacyConsent() { accepts++; rec.record = { version: '1.0' }; return true; },
      WithdrawPrivacyConsent() { rec.record = null; return true; },
      StartPreview() { starts++; return true; },
      StopPreview() {}
    } });
    if (action === 'withdraw') {
      p.element('consent-agree').checked = true;
      p.context.onConsentAccept();
    }
    p.app.consent[action]();
    const before = starts;
    p.context.onConsentStatus();
    assert.equal(p.element('modal-mid').textContent, '重新同意');
    p.context.onModalMid();
    assert.ok(p.element('consent-overlay').classList.contains('active'));
    assert.equal(p.element('consent-agree').checked, false);
    p.context.onConsentAccept();
    assert.equal(starts, before);
    p.element('consent-agree').checked = true;
    p.context.onConsentAccept();
    assert.equal(starts, before + 1);
    assert.equal(accepts, action === 'withdraw' ? 2 : 1);
    assert.equal(p.app.captureCtl.isActive(), false);
  });
}

// The overlay handoff: cam-loading is only dismissed by the FIRST frame of a
// viewing session. A withdraw re-shows the overlay; the re-grant must re-arm
// the handoff or the stale "camera ready" from the first session leaves
// "正在启动摄像头" stuck over working video (real-machine report on 5F0ECB5A).
test('re-granting consent after a withdrawal clears the starting-camera overlay', () => {
  const rec = { noticeVersion: '1.0', record: null };
  let frame = 0;
  const p = page({ grantConsent: false, host: {
    GetPrivacyConsent: () => JSON.stringify(rec),
    AcceptPrivacyConsent() { rec.record = { version: '1.0', time: '2026-01-02T03:04:05+08:00' }; return true; },
    WithdrawPrivacyConsent() { rec.record = null; return true; },
    StartPreview: () => true,
    StopPreview() {},
    GetLatestFrameAndFaces: () => String(++frame) + '\x1edata:image/jpeg;base64,F' + frame + '\x1e[]'
  } });
  // Drain each pending timer exactly once. The mock keeps fired entries, so a
  // plain values() loop would re-run stale timers on a later drain — the first
  // session's 350ms cam-loading hide firing a second time at the re-grant is
  // exactly what masked this bug in an earlier version of this test.
  const flush = () => {
    for (const [key, fn] of [...p.timeouts.entries()]) {
      if (p.timeouts.delete(key)) fn();
    }
  };

  // First session: accept the gate; the first frame marks the camera ready
  // and the deferred hide settles the startup overlay. The camera start is
  // itself deferred (app_main pushes StartPreview past the page reveal), so
  // it runs while draining one flush and only then registers the 350ms
  // overlay hide — a second flush settles it.
  p.element('consent-agree').checked = true;
  p.context.onConsentAccept();
  flush();
  flush();
  assert.equal(p.app.previewCtl.isViewing(), true);
  assert.equal(p.element('cam-loading').style.display, 'none');

  // Withdraw: camera and loop down, overlay back up in the blocked state.
  p.app.consent.withdraw();
  assert.equal(p.app.previewCtl.isViewing(), false);
  assert.equal(p.element('cam-loading').style.display, 'flex');

  // Re-grant through the pill's dialog, exactly the reported path.
  p.context.onConsentStatus();
  p.context.onModalMid();
  p.element('consent-agree').checked = true;
  p.context.onConsentAccept();
  assert.equal(p.app.previewCtl.isViewing(), true);
  assert.equal(p.element('cam-loading').style.display, 'flex'); // hide is deferred
  flush();
  assert.equal(p.element('cam-loading').style.display, 'none'); // was stuck here
});

test('page cancellation and retry cannot start an old sequence angle', () => {
  const started = [];
  const p = page({ host: { CaptureFaceSamples(angle) { started.push(angle); return true; } } });
  const { context, intervals, timeouts } = p;
  // Skip unrelated page-start timers; exercise real page event handlers.
  intervals.clear(); timeouts.clear();
  context.onCapture();
  [...intervals.values()][0](); // first angle completes, next angle is queued
  const queued = [...timeouts.values()];
  context.onCapture(); // capture in flight — the same button doubles as cancel
  context.onCapture();
  queued.forEach(fn => fn());
  assert.deepEqual(started, [0, 0]);
  assert.equal(p.app.captureCtl.isActive(), true);
  p.state.status = { done: true, livenessPassed: false, collected: 0, target: 5 };
  [...intervals.values()][0]();
  assert.equal(p.app.captureCtl.isActive(), false);
  assert.equal(intervals.size, 0);
  assert.equal(p.element('cam-caption').textContent, '采集失败');
});

test('switching to the faces tab loads and renders the face list', () => {
  const p = page();
  p.context.switchTab('faces');
  assert.equal(p.calls.facesJson, 1);
  assert.ok(p.element('face-list').innerHTML.indexOf('test-face') >= 0);
  assert.equal(p.element('faces-count').textContent, '1 / 3 张');
  assert.equal(p.app.nav.tab(), 'faces');
  assert.ok(p.element('screen-faces').classList.contains('active'));
  assert.ok(!p.element('screen-cam').classList.contains('active'));
});

test('showScreen swaps flow screens directly and keeps the tab tracker on cam', () => {
  const p = page();
  p.app.nav.showScreen('pwd');    // finishCapture's first-enrollment branch
  assert.ok(p.element('screen-pwd').classList.contains('active'));
  assert.equal(p.app.nav.tab(), 'cam');   // flow screen — tracker unmoved
  p.app.nav.showScreen('cam');    // save/cancel return path
  assert.ok(p.element('screen-cam').classList.contains('active'));
  assert.equal(p.app.nav.tab(), 'cam');
});

test('saving settings round-trips non-UI config keys through the loaded snapshot', () => {
  const p = page();
  p.app.settings.load();
  assert.equal(p.element('cfg-threshold').value, 0.85);
  assert.equal(p.element('cfg-threshold-val').textContent, '0.85');
  p.element('cfg-rotation').value = '180';
  p.app.settings.save();
  [...p.timeouts.values()].forEach(fn => fn()); // run the deferred BeginConfigSave
  assert.equal(p.calls.setConfig.length, 1);
  const saved = JSON.parse(p.calls.setConfig[0]);
  assert.equal(saved.camera_rotation, 180);   // UI-managed key overridden
  assert.equal(saved.secret_key, 'keep-me');  // non-UI key survives the round trip
  assert.equal(p.element('btn-save-settings').disabled, false); // withBusy restored the button
});

test('unlock tier selector maps config frames to the matching radio and saves the picked tier', () => {
  const p = page();
  p.state.config.unlock_frames = 5;
  p.app.settings.load();
  assert.equal(p.element('cfg-unlockframes-0').checked, true);   // strict cell on
  assert.equal(p.element('cfg-unlockframes-1').checked, false);
  p.app.settings.onUnlockTierInput('2');                          // user picks 极速
  p.app.settings.save();
  [...p.timeouts.values()].forEach(fn => fn());
  assert.equal(JSON.parse(p.calls.setConfig[0]).unlock_frames, 2);
});

test('unknown unlock_frames config snaps the selector back to standard', () => {
  const p = page();
  p.state.config.unlock_frames = 4;
  p.app.settings.load();
  assert.equal(p.element('cfg-unlockframes-1').checked, true);
  assert.equal(p.element('cfg-unlockframes-0').checked, false);
  p.app.settings.save();
  [...p.timeouts.values()].forEach(fn => fn());
  assert.equal(JSON.parse(p.calls.setConfig[0]).unlock_frames, 3);
});

test('banner re-show clears the previous hide timer for the same screen only', () => {
  const p = page();
  p.timeouts.clear(); // drop the page-start timers, keep only banner timers
  p.app.ui.showBanner('cam', 'first', 'ok');
  const camTimer = [...p.timeouts.keys()][0];
  p.app.ui.showBanner('settings', 'other', 'error');
  assert.equal(p.timeouts.size, 2);
  p.app.ui.showBanner('cam', 'second', 'ok'); // clears cam's old timer, not settings'
  assert.equal(p.timeouts.size, 2);            // one cleared, one added
  assert.ok(!p.timeouts.has(camTimer));
  [...p.timeouts.values()].forEach(fn => fn());
  assert.equal(p.element('cam-banner').className, 'banner');
  assert.equal(p.element('settings-banner').className, 'banner');
});

test('modal ok hands the input value to the callback and closes the dialog', () => {
  const p = page();
  let got = null;
  p.app.ui.showAppModal({ title: 't', message: 'm', onOk(v) { got = v; } });
  assert.ok(p.element('modal-overlay').classList.contains('active'));
  p.element('modal-input').value = 'hello';
  p.app.ui.onModalOk();
  assert.equal(got, 'hello');
  assert.ok(!p.element('modal-overlay').classList.contains('active'));
});

// The mock has no matchMedia, so "system" resolves to light; the attribute
// always reflects the resolved theme.
test('theme defaults to system; picks pin the page and persist through the host', () => {
  const wrote = [];
  const p = page({ host: {
    GetThemeMode: () => 'system',
    SetThemeMode(m) { wrote.push(m); return true; }
  }});
  const root = p.element('html');
  assert.equal(root.getAttribute('data-theme'), 'light');
  assert.ok(p.element('theme-row-system').classList.contains('on'));
  p.context.onThemePick('dark');
  assert.deepEqual(wrote, ['dark']);
  assert.equal(root.getAttribute('data-theme'), 'dark');
  assert.ok(p.element('theme-row-dark').classList.contains('on'));
  p.context.onThemePick('light');
  assert.equal(root.getAttribute('data-theme'), 'light');
  p.context.onThemePick('system');
  assert.equal(root.getAttribute('data-theme'), 'light');
  assert.deepEqual(wrote, ['dark', 'light', 'system']);
});

test('a persisted dark preference applies at startup', () => {
  const p = page({ host: { GetThemeMode: () => 'dark' } });
  assert.equal(p.element('html').getAttribute('data-theme'), 'dark');
  assert.ok(p.element('theme-row-dark').classList.contains('on'));
});

function runDeferred(p) {
  const callbacks = [...p.timeouts.values()];
  p.timeouts.clear();
  callbacks.forEach(fn => fn());
}

test('log clear failures preserve the displayed log and report failure for every source', () => {
  for (const src of ['console', 'service', 'worker', 'cp']) {
    const p = page({ host: { ClearLog: () => false, ClearFileLog: () => false } });
    p.element('log-source').value = src;
    p.element('log-viewer').textContent = 'keep these logs';
    p.app.logs.clearLogs();
    assert.equal(p.element('log-viewer').textContent, 'keep these logs');
    assert.match(p.element('btn-clear-log').textContent, /失败/);
  }
});

test('log clear exceptions report failure and successful clears empty the viewer', () => {
  for (const throws of [false, true]) {
    const p = page({ host: { ClearFileLog() {
      if (throws) throw new Error('file unavailable');
      return true;
    } } });
    p.element('log-source').value = 'service';
    p.element('log-viewer').textContent = 'visible logs';
    p.app.logs.clearLogs();
    assert.equal(p.element('log-viewer').textContent, throws ? 'visible logs' : '');
    assert.match(p.element('btn-clear-log').textContent, throws ? /失败/ : /已清空/);
  }
});

test('unpersisted face mutations keep the list and report failure rather than a stale reload result', () => {
  const p = page({ host: { DeleteFace: () => false, GetAccountSaveStatus: () => 'failed' } });
  p.app.faces.onDeleteFace(1);
  p.app.ui.onModalOk();
  assert.equal(p.calls.facesJson, 0);
  assert.match(p.element('faces-banner').textContent, /删除失败/);
});

test('persisted face management operations refresh the list even without service acknowledgement', () => {
  for (const action of ['onDeleteFace', 'onClearAllFaces', 'onRenameFace']) {
    const p = page({ host: { DeleteFace: () => false, ClearAllFaces: () => false,
      RenameFace: () => false, GetAccountSaveStatus: () => 'reload_failed' } });
    p.app.faces[action](1);
    p.element('modal-input').value = 'new label';
    p.app.ui.onModalOk();
    assert.equal(p.calls.facesJson, 1);
    assert.match(p.element('faces-banner').textContent, /已保存.*未确认/);
  }
});

test('capture failure reasons provide camera, face and model guidance instead of blaming liveness', () => {
  for (const [reason, expected] of [
    ['no_frames', /摄像头.*画面/], ['no_face', /未检测到人脸/],
    ['inference_failed', /模型|推理/], ['liveness_timeout', /未能完成活体检测/],
    ['liveness_failed', /活体检测未通过/]
  ]) {
    const p = page();
    p.element('chk-multi-angle').checked = false;
    p.app.captureUi.onCapture();
    p.state.status = { done: true, livenessPassed: false, collected: 0, target: 5,
      livenessChecking: false, failureReason: reason };
    [...p.intervals.values()].forEach(fn => fn());
    assert.equal(p.app.captureCtl.isActive(), false);
    assert.match(p.element('cam-banner').textContent, expected);
  }
});

test('a storage failure after valid password is not reported as a password error', () => {
  const p = page({ host: { ValidatePassword: () => true, SaveEnrollment: () => false,
    GetEnrollmentSaveStatus: () => 'failed' } });
  p.element('fld-pass').value = 'synthetic-password';
  p.element('fld-acct-type').value = 'local';
  p.app.enrollSave.onSave();
  runDeferred(p);
  assert.match(p.element('pwd-banner').textContent, /保存失败/);
  assert.doesNotMatch(p.element('pwd-banner').textContent, /密码错误/);
});

test('a persisted enrollment without reload acknowledgement exits the consumed-sample form', () => {
  const p = page({ host: { ValidatePassword: () => true, SaveEnrollment: () => false,
    GetEnrollmentSaveStatus: () => 'reload_failed', StartPreview: () => true } });
  p.element('fld-pass').value = 'synthetic-password';
  p.app.enrollSave.onSave();
  runDeferred(p);
  assert.ok(p.element('screen-cam').classList.contains('active'));
  assert.equal(p.element('fld-pass').value, '');
  assert.match(p.element('cam-banner').textContent, /已保存.*未确认/);
});

test('append also reports persisted faces without asking the user to save consumed samples again', () => {
  const p = page({ host: { SaveEnrollmentAppend: () => false,
    GetEnrollmentSaveStatus: () => 'reload_failed', StartPreview: () => true } });
  p.app.enrollSave.onAppendConfirm();
  runDeferred(p);
  assert.ok(p.element('screen-cam').classList.contains('active'));
  assert.match(p.element('cam-banner').textContent, /已保存.*未确认/);
});

test('returning to capture reports both false and throwing camera starts', () => {
  for (const start of [() => false, () => { throw new Error('camera unavailable'); }]) {
    const p = page({ host: { StartPreview: start, StopPreview() {} } });
    p.app.nav.switchTab('settings');
    p.app.nav.switchTab('cam');
    assert.match(p.element('cam-banner').textContent, /预览启动失败/);
    assert.equal(p.app.previewCtl.isViewing(), false);
    assert.equal(p.element('cam-loading').style.display, 'none');
  }
});

test('settings save stays busy until the background reload completes and rejects repeat saves', () => {
  let starts = 0, status = 'pending';
  const p = page({ host: {
    BeginConfigSave() { starts++; return true; }, GetConfigSaveStatus: () => status
  } });
  p.app.settings.load();
  p.app.settings.save();
  runDeferred(p);
  assert.equal(starts, 1);
  assert.equal(p.element('btn-save-settings').disabled, true);
  assert.equal(p.element('settings-spinner').style.display, 'inline-block');
  p.app.settings.save();
  assert.equal(starts, 1);
  p.app.nav.switchTab('logs'); // navigation remains available during the wait
  status = 'saved';
  runDeferred(p);
  assert.equal(p.element('btn-save-settings').disabled, false);
  assert.equal(p.element('settings-spinner').style.display, 'none');
  assert.match(p.element('settings-banner').textContent, /已保存/);
});

test('camera picker reload failure is shown inside the open picker and can be retried', () => {
  let status = 'pending';
  const p = page({ host: { BeginConfigSave: () => true, GetConfigSaveStatus: () => status } });
  p.element('cam-picker-overlay').classList.add('active');
  p.element('cam-picker-select').value = 'camera-a';
  p.app.settings.onCamPickerConfirm();
  runDeferred(p);
  assert.equal(p.element('cam-picker-confirm').disabled, true);
  assert.equal(p.element('cam-picker-skip').disabled, true);
  status = 'reload_failed';
  runDeferred(p);
  assert.ok(p.element('cam-picker-overlay').classList.contains('active'));
  assert.match(p.element('cam-picker-error').textContent, /已保存.*未确认/);
  assert.equal(p.element('cam-picker-confirm').disabled, false);
  assert.equal(p.element('cam-picker-skip').disabled, false);
  status = 'saved';
  p.app.settings.onCamPickerConfirm();
  runDeferred(p);
  assert.equal(p.element('cam-picker-error').textContent, '');
  assert.equal(p.element('cam-picker-overlay').classList.contains('active'), false);
});

test('invalid passwords retain account-specific guidance and never attempt a save', () => {
  for (const type of ['local', 'msa']) {
    let saves = 0;
    const p = page({ host: { ValidatePassword: () => false,
      SaveEnrollment() { saves++; return true; } } });
    p.element('fld-pass').value = 'synthetic-password';
    p.element('fld-acct-type').value = type;
    p.app.enrollSave.onSave();
    runDeferred(p);
    assert.equal(saves, 0);
    assert.match(p.element('pwd-banner').textContent,
      type === 'msa' ? /微软账号密码/ : /密码错误/);
    assert.equal(p.element('btn-save-cancel').disabled, false);
  }
});

test('settings restore controls and distinguish write failure, rejected reload and bridge errors', () => {
  for (const scenario of ['write', 'reload', 'throw', 'poll-throw']) {
    const p = page({ host: {
      BeginConfigSave() {
        if (scenario === 'throw') throw new Error('bridge failed');
        return scenario !== 'write';
      },
      GetConfigSaveStatus() {
        if (scenario === 'poll-throw') throw new Error('bridge failed');
        return 'reload_failed';
      }
    } });
    p.app.settings.load();
    p.app.settings.save();
    runDeferred(p);
    assert.equal(p.element('btn-save-settings').disabled, false);
    assert.equal(p.element('settings-spinner').style.display, 'none');
    assert.match(p.element('settings-banner').textContent,
      scenario === 'write' ? /保存失败/ : scenario === 'reload' ? /已保存.*未确认/ : /无法确认/);
  }
});

test('camera picker reports read and write failures inside its overlay', () => {
  for (const scenario of ['read', 'write']) {
    const p = page({ host: {
      GetConfig() { if (scenario === 'read') throw new Error('read failed'); return '{}'; },
      BeginConfigSave: () => false
    } });
    p.element('cam-picker-overlay').classList.add('active');
    p.element('cam-picker-select').value = 'camera-a';
    p.app.settings.onCamPickerConfirm();
    runDeferred(p);
    assert.match(p.element('cam-picker-error').textContent, /无法读取|保存失败/);
    assert.equal(p.element('cam-picker-confirm').disabled, false);
    assert.ok(p.element('cam-picker-overlay').classList.contains('active'));
  }
});

test('disabled camera select facade cannot open or commit a changed selection', () => {
  function node() {
    return {
      children: [], events: {}, attrs: {}, style: {}, classList: { add() {}, remove() {} },
      appendChild(child) { this.children.push(child); child.parentNode = this; },
      insertBefore(child) { this.appendChild(child); },
      addEventListener(name, fn) { this.events[name] = fn; },
      setAttribute(name, value) { this.attrs[name] = value; },
      getBoundingClientRect: () => ({ left: 0, top: 0, bottom: 20, width: 100 })
    };
  }
  const sel = node(), parent = node();
  parent.appendChild(sel);
  sel.options = [{ textContent: 'A' }, { textContent: 'B' }];
  sel.selectedIndex = 0;
  sel.disabled = false;
  let changes = 0;
  sel.dispatchEvent = () => { changes++; };
  const context = vm.createContext({
    document: { createElement: node, querySelectorAll: () => [sel], addEventListener() {} },
    window: { addEventListener() {}, innerHeight: 700 }, Event: function() {}
  });
  vm.runInContext(fs.readFileSync(path.join(__dirname,
    '../../enrollment_app/js/custom_select.js'), 'utf8'), context);
  const selects = context.createCustomSelectModule();
  selects.enhanceAll();
  const trigger = sel.parentNode.children[1], popup = sel.parentNode.children[2];
  const event = { stopPropagation() {}, preventDefault() {} };
  trigger.events.click(event);
  assert.equal(popup.style.display, 'block');
  sel.disabled = true;
  selects.refreshAll();
  assert.equal(popup.style.display, 'none');
  assert.equal(trigger.attrs['aria-disabled'], 'true');
  assert.equal(trigger.tabIndex, -1);
  popup.children[1].events.click(event); // an already queued option click
  trigger.events.keydown({ ...event, key: 'ArrowDown' });
  assert.equal(sel.selectedIndex, 0);
  assert.equal(changes, 0);
  assert.equal(popup.style.display, 'none');
  sel.disabled = false;
  selects.refreshAll();
  trigger.events.click(event);
  popup.children[1].events.click(event);
  assert.equal(sel.selectedIndex, 1);
  assert.equal(changes, 1);
});


test('English capture keeps stored Chinese angle keys and finds the missing slot', () => {
  const p = page({host: {GetUiLanguage: () => 'en'}});
  assert.equal(p.app.captureCtl.angleLabel(0), 'Front');
  assert.equal(p.app.captureCtl.angleLabel(1), 'Left');
  assert.equal(p.app.captureCtl.firstMissingAngle({'正面': true, '左转': true}), 2);
  assert.equal(p.app.captureCtl.instr(1), 'Turn left about 30°');
  assert.equal(p.context.lfStoredLabel('My own face name'), 'My own face name');
  assert.equal(p.context.lfT('capture_count', {count: 5}), '5 frames captured');
});

test('English service reload failure remains a saved enrollment rather than a retry', () => {
  const p = page({host: {GetUiLanguage: () => 'en',
    GetEnrollmentSaveStatus: () => 'reload_failed', SaveEnrollment: () => false,
    StartPreview: () => true, ValidatePassword: () => true}});
  p.element('fld-pass').value = 'synthetic-test-password';
  p.app.enrollSave.onSave();
  for (const fn of [...p.timeouts.values()]) fn();
  assert.match(p.element('cam-banner').textContent, /hasn.t confirmed loading/);
  assert.equal(p.element('fld-pass').value, '');
});

test('English consent does not resume a pending withdrawal', () => {
  const p = page({grantConsent: false, host: {GetUiLanguage: () => 'en',
    GetPrivacyConsent: () => JSON.stringify({noticeVersion: '1.1', withdrawalPending: true})}});
  assert.equal(p.app.consent.granted(), false);
  p.app.consent.refreshStatus();
  assert.equal(p.element('btn-consent-status').textContent, 'Withdrawal pending');
});

test('language changes persist through the host and report failure without changing active UI', () => {
  const saved = [];
  const p = page({host: {GetUiLanguage: () => 'en',
    SetLanguageMode: mode => {saved.push(mode); return mode === 'zh-CN';}}});
  p.context.onLanguagePick('zh-CN');
  assert.deepEqual(saved, ['zh-CN']);
  assert.equal(p.element('language-status').textContent, 'Language saved');
  assert.equal(p.element('language-row-zh-CN').getAttribute('aria-checked'), 'true');
  assert.equal(p.element('language-row-system').getAttribute('aria-checked'), 'false');
  assert.equal(p.context.lfLocale, 'en');
  p.context.onLanguagePick('bad-language');
  assert.match(p.element('language-status').textContent, /Unable to save language/);
  assert.equal(p.element('language-row-zh-CN').getAttribute('aria-checked'), 'true');
});
