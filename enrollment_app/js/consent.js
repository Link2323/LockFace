// Per-user privacy consent gate. The installer consent is machine-wide and may
// have been accepted by a different Windows user; this gate is shown before the
// camera is allowed to start and is the actual consent of the person whose face
// will be enrolled. deps: {previewCtl, ui}.
function createConsentGate(deps) {
  var previewCtl = deps.previewCtl;
  var ui = deps.ui;
  var granted = false;
  var pending = null;

  function overlay() { return document.getElementById('consent-overlay'); }
  function agreeBox() { return document.getElementById('consent-agree'); }
  function acceptBtn() { return document.getElementById('btn-consent-accept'); }

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
    grant();
    markCameraPending();
    var fn = pending;
    pending = null;
    if (fn) fn();
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
    openPrivacyNotice: openPrivacyNotice,
    granted: function() { return granted; }
  };
}
