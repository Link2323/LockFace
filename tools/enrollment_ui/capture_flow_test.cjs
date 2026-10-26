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
      addEventListener() {}, querySelector: () => null, getContext: () => canvasContext()
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
    SetConfig(json) { calls.setConfig.push(json); return true; }
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
  }, showBanner() {} });
  preview.setConsentGranted(true);
  preview.restartPreview();
  return { counts, viewport, state, context, preview, loading, transport,
    imageRequests: () => imageRequests,
    load() { image.complete = true; image.onload(); },
    poll() {
      const entry = [...pending].find(([, fn]) => fn.name === 'renderFrame');
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

test('the consent pill opens a status dialog with view and withdraw actions', () => {
  const rec = { noticeVersion: '1.0', record: { version: '1.0', time: '2026-01-02T03:04:05+08:00' } };
  const p = page({ host: {
    GetPrivacyConsent: () => JSON.stringify(rec)
  } });
  p.context.onConsentStatus();
  assert.ok(p.element('modal-overlay').classList.contains('active'));
  assert.equal(p.element('modal-ok').textContent, '查看隐私说明');
  assert.equal(p.element('modal-mid').textContent, '撤回同意');
  assert.ok(p.element('modal-message').textContent.indexOf('你已同意') === 0);
  p.context.onModalCancel();
  assert.ok(!p.element('modal-overlay').classList.contains('active'));
});

test('the status dialog offers consent without a current record', () => {
  const p = page({ host: {
    GetPrivacyConsent: () => JSON.stringify({ noticeVersion: '1.0', record: null })
  } });
  p.app.consent.statusMenu();
  assert.equal(p.element('modal-message').textContent,
               '尚未同意隐私说明；摄像头和采集启动前会先征求同意。');
  assert.equal(p.element('modal-mid').textContent, '重新同意');
  assert.notEqual(p.element('modal-mid').style.display, 'none');
});

for (const action of ['decline', 'withdraw']) {
  test(`${action} allows consent to be reopened and preview resumed from the status button`, () => {
    let starts = 0, accepts = 0;
    const rec = { noticeVersion: '1.0', record: null };
    const p = page({ grantConsent: false, host: {
      GetPrivacyConsent: () => JSON.stringify(rec),
      AcceptPrivacyConsent() { accepts++; rec.record = { version: '1.0' }; },
      WithdrawPrivacyConsent() { rec.record = null; },
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
  [...p.timeouts.values()].forEach(fn => fn()); // run the deferred SetConfig
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
