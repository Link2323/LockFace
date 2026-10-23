// Per-user privacy consent gate. The installer consent is machine-wide and may
// have been accepted by a different Windows user; this gate is shown before
// the camera is allowed to start and is the actual consent of the person whose
// face will be enrolled. Acceptance is remembered per Windows user through the
// host (HKCU record: notice version + time) and silently resumed while the
// shipped PRIVACY.txt still carries the accepted 版本 line; a notice revision,
// a missing record or an explicit withdrawal (settings screen) reopens the
// gate. deps: {host, previewCtl, ui}.
function createConsentGate(deps) {
  var H = deps.host;
  var previewCtl = deps.previewCtl;
  var ui = deps.ui;
  var granted = false;
  var pending = null;

  function overlay() { return document.getElementById('consent-overlay'); }
  function agreeBox() { return document.getElementById('consent-agree'); }
  function acceptBtn() { return document.getElementById('btn-consent-accept'); }

  // Host-side acceptance state, or null when the method is unavailable
  // (headless test page) or the record cannot be read — callers treat null as
  // "no remembered consent" and show the gate.
  function hostState() {
    try { return JSON.parse(H.GetPrivacyConsent()); } catch(e) { return null; }
  }

  function remembered() {
    var st = hostState();
    return !!(st && st.record && st.record.version &&
              st.noticeVersion && st.record.version === st.noticeVersion);
  }

  function syncButton() {
    var btn = acceptBtn();
    if (btn) btn.disabled = !(agreeBox() && agreeBox().checked);
  }

  function setCaption(text) {
    var el = document.getElementById('cam-caption');
    if (el) el.textContent = text;
  }

  function markCameraPending() {
    var loading = document.getElementById('cam-loading');
    if (!loading) return;
    loading.style.display = 'flex';
    loading.classList.remove('fade');
    var spinner = loading.querySelector('.spinner-dark');
    if (spinner) spinner.style.display = '';
    var text = loading.querySelector('.loading-text');
    if (text) text.textContent = '正在启动摄像头...';
  }

  function markCameraBlocked() {
    var loading = document.getElementById('cam-loading');
    if (!loading) return;
    loading.style.display = 'flex';
    loading.classList.remove('fade');
    var spinner = loading.querySelector('.spinner-dark');
    if (spinner) spinner.style.display = 'none';
    var text = loading.querySelector('.loading-text');
    if (text) text.textContent = '摄像头未启动：需要先同意隐私说明';
  }

  function show(fn) {
    pending = fn || null;
    var el = overlay();
    if (!el) return;           // fail closed: no consent UI means no camera
    el.classList.add('active');
    if (!granted) {
      var box = agreeBox();
      if (box) box.checked = false;
      syncButton();
    }
  }

  // Mark the gate as accepted without running the pending action. accept()
  // uses this in production; the headless page test uses it to skip the modal.
  function grant() {
    granted = true;
    previewCtl.setConsentGranted(true);
    var el = overlay();
    if (el) el.classList.remove('active');
  }

  function accept() {
    var box = agreeBox();
    if (!granted && !(box && box.checked)) return;
    var firstAcceptance = !granted;
    grant();
    // Persist the acceptance (notice version + time). The just-given consent
    // is real regardless — a failed write only means the gate returns on the
    // next launch.
    if (firstAcceptance) {
      try { H.AcceptPrivacyConsent(); } catch(e) {}
      refreshStatus();
    }
    markCameraPending();
    var fn = pending;
    pending = null;
    if (fn) fn();
  }

  // Settings-screen withdrawal: clear the remembered record, revoke this
  // session's grant and power the camera down. The gate reappears on the next
  // capture attempt or launch.
  function withdraw() {
    try { H.WithdrawPrivacyConsent(); } catch(e) {}
    granted = false;
    previewCtl.setConsentGranted(false);
    pending = null;
    var el = overlay();
    if (el) el.classList.remove('active');
    previewCtl.stopLoop();
    try { H.StopPreview(); } catch(e) {}
    markCameraBlocked();
    setCaption('需要同意隐私说明后才能启动摄像头。');
    refreshStatus();
    ui.showBanner('cam', '已撤回隐私同意。摄像头已停止，下次使用时会重新征求同意。', 'ok', 8000);
  }

  function fmtTime(t) {
    if (typeof t !== 'string' || t.length < 16) return '未知';
    return t.slice(0, 10) + ' ' + t.slice(11, 16);
  }

  // Consent state derived from the remembered record: {st, has, current}.
  function readState() {
    var st = hostState();
    var has = !!(st && st.record && st.record.version);
    var current = !!(has && st.noticeVersion && st.record.version === st.noticeVersion);
    return { st: st, has: has, current: current };
  }

  // Paint the consent pill in the option row above the capture button. Plain-language states
  // only — the registry's notice version stays in the record, not the UI.
  function refreshStatus() {
    var pill = document.getElementById('btn-consent-status');
    if (!pill) return;
    var s = readState();
    if (s.current) {
      pill.textContent = '已同意隐私说明';
      pill.className = 'consent-pill state-ok';
      pill.title = '已同意《LockFace 人脸识别隐私说明与知情同意书》，点击可查看或撤回';
    } else if (s.has) {
      pill.textContent = '隐私说明需重新同意';
      pill.className = 'consent-pill state-warn';
      pill.title = '隐私说明有更新，重新同意后摄像头才会启动，点击查看详情';
    } else {
      pill.textContent = '未同意隐私说明';
      pill.className = 'consent-pill';
      pill.title = '同意后才能启动摄像头和采集，点击查看详情';
    }
  }

  // The pill's click action: a status dialog with 查看隐私说明 (primary) and,
  // only while the current notice version is accepted, 撤回同意 (secondary).
  // hide-then-callback in the modal engine lets 查看开 the license viewer.
  function statusMenu() {
    var s = readState();
    var opts = {
      title: '隐私同意',
      okText: '查看隐私说明',
      cancelText: '关闭',
      onOk: openPrivacyNotice
    };
    if (s.current) {
      opts.message = '你已同意《LockFace 人脸识别隐私说明与知情同意书》' +
                     '（' + fmtTime(s.st.record.time) + '）。摄像头与人脸采集在此同意下运行；' +
                     '隐私说明更新后会重新征求同意。';
      opts.midText = '撤回同意';
      opts.onMid = withdraw;
    } else if (s.has) {
      opts.message = '隐私说明有更新。重新同意后摄像头才会启动；下次采集时会先弹出同意。';
    } else if (s.st) {
      opts.message = '尚未同意隐私说明；摄像头和采集启动前会先征求同意。';
    } else {
      opts.message = '同意记录暂时读取不到；下次启动会重新征求同意。';
    }
    ui.showAppModal(opts);
  }

  function decline() {
    var el = overlay();
    if (el) el.classList.remove('active');
    pending = null;
    markCameraBlocked();
    setCaption('需要同意隐私说明后才能启动摄像头。');
    ui.showBanner('cam', '你尚未同意隐私说明，摄像头不会启动。需要录入时请点击“开始采集”。', 'error', 8000);
  }

  // Returns true when already granted. Otherwise shows the gate, remembers fn
  // as the resume action, and returns false so the caller stops immediately.
  function ensure(fn) {
    if (granted) return true;
    show(fn);
    return false;
  }

  function init(fn) {
    var box = agreeBox();
    if (box) box.addEventListener('change', syncButton);
    refreshStatus();
    // A remembered acceptance of the current notice version resumes without
    // the dialog: grant and run the startup callback exactly like accept().
    if (remembered()) {
      grant();
      markCameraPending();
      if (fn) fn();
      return;
    }
    show(fn);
  }

  function openPrivacyNotice() {
    ui.openLicenseViewer('PRIVACY.txt');
  }

  return {
    init: init,
    ensure: ensure,
    accept: accept,
    decline: decline,
    grant: grant,
    withdraw: withdraw,
    refreshStatus: refreshStatus,
    statusMenu: statusMenu,
    openPrivacyNotice: openPrivacyNotice,
    granted: function() { return granted; }
  };
}
