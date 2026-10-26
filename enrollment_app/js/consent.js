// Per-user biometric consent gate. The installation notice acknowledgment is
// machine-wide and may belong to a different user; this gate is shown before
// the camera is allowed to start and is the actual consent of the person whose
// face will be enrolled. Acceptance is remembered per Windows user through the
// host (HKCU record: notice version + time) and silently resumed while the
// notice version matches and no withdrawal is pending. A persistent withdrawal
// marker takes priority over acceptance; only a confirmed deletion permits
// fresh consent. deps: {host, previewCtl, ui, onWithdraw}.
function createConsentGate(deps) {
  var H = deps.host;
  var previewCtl = deps.previewCtl;
  var ui = deps.ui;
  var setCaption = ui.setCaption;
  var granted = false;
  var pending = null;
  var withdrawalFailed = false;
  var generation = 0;
  var consentRequiredText = '未同意隐私说明，摄像头已停止。';

  // Deferred work belongs to the consent under which it was scheduled. A
  // later acceptance does not revive work queued before a withdrawal.
  function guardCurrent(fn) {
    var expected = generation;
    return function() {
      if (granted && generation === expected && !readState().blocked) {
        return fn.apply(null, arguments);
      }
    };
  }

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
    return !!(!withdrawalFailed && st && !st.withdrawalPending && st.record && st.record.version &&
              st.noticeVersion && st.record.version === st.noticeVersion);
  }

  function syncButton() {
    var btn = acceptBtn();
    if (btn) btn.disabled = !(agreeBox() && agreeBox().checked);
  }

  function markCameraPending() {
    setCaption('正在启动摄像头...');
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
    setCaption(consentRequiredText);
    var loading = document.getElementById('cam-loading');
    if (!loading) return;
    loading.style.display = 'flex';
    loading.classList.remove('fade');
    var spinner = loading.querySelector('.spinner-dark');
    if (spinner) spinner.style.display = 'none';
    var text = loading.querySelector('.loading-text');
    if (text) text.textContent = consentRequiredText + '点击“开始采集”或隐私状态按钮可重新同意。';
  }

  function showConsentRequired() {
    if (readState().blocked) markWithdrawalPending();
    else markCameraBlocked();
  }

  function show(fn) {
    if (readState().blocked) {
      pending = null;
      markWithdrawalPending();
      statusMenu();
      return;
    }
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
    if (!granted) generation++;
    granted = true;
    previewCtl.setConsentGranted(true);
    var el = overlay();
    if (el) el.classList.remove('active');
  }

  function accept() {
    if (readState().blocked) {
      markWithdrawalPending();
      statusMenu();
      return;
    }
    var box = agreeBox();
    if (!granted && !(box && box.checked)) return;
    var firstAcceptance = !granted;
    // A failed host write can indicate a pending withdrawal. Do not grant
    // camera access until the native side has accepted and stored consent.
    if (firstAcceptance) {
      var accepted = false;
      try { accepted = H.AcceptPrivacyConsent() === true; } catch(e) {}
      if (!accepted) {
        ui.showBanner('cam', '未能保存同意，请先完成待处理的撤回，或检查权限后重试。摄像头保持关闭。', 'error', 15000);
        refreshStatus();
        return;
      }
      refreshStatus();
    }
    ui.hideBanner('cam');
    grant();
    markCameraPending();
    var fn = pending;
    pending = null;
    if (fn) fn();
  }

  // Always stop local processing. Only confirmed service deletion and registry
  // cleanup count as a completed withdrawal; the host keeps a separate retry marker.
  function withdraw() {
    generation++;
    if (deps.onWithdraw) deps.onWithdraw();
    var password = document.getElementById('fld-pass');
    if (password) password.value = '';
    granted = false;
    previewCtl.setConsentGranted(false);
    pending = null;
    var el = overlay();
    if (el) el.classList.remove('active');
    previewCtl.stopLoop();
    try { H.StopPreview(); } catch(e) {}
    var confirmed = false;
    try { confirmed = H.WithdrawPrivacyConsent() === true; } catch(e) {}
    withdrawalFailed = !confirmed;
    markCameraBlocked();
    refreshStatus();
    if (confirmed) {
      ui.showBanner('cam', '已撤回同意，当前账户的人脸、保存的密码和旧备份已清理。', 'ok', 12000);
    } else {
      markWithdrawalPending();
      ui.showBanner('cam', '摄像头已停止，但未能确认撤回及数据删除完成。请检查服务状态并重试；此前的人脸登录可能仍可用。', 'error', 20000);
    }
  }

  function markWithdrawalPending() {
    if (granted) generation++;
    granted = false;
    previewCtl.setConsentGranted(false);
    previewCtl.stopLoop();
    try { H.StopPreview(); } catch(e) {}
    var el = overlay();
    if (el) el.classList.remove('active');
    markCameraBlocked();
    var loading = document.getElementById('cam-loading');
    var text = loading && loading.querySelector('.loading-text');
    if (text) text.textContent = '摄像头未启动：撤回处理中';
    setCaption('撤回处理中：请完成数据删除后再重新同意。');
  }

  function fmtTime(t) {
    if (typeof t !== 'string' || t.length < 16) return '未知';
    return t.slice(0, 10) + ' ' + t.slice(11, 16);
  }

  // Consent state derived from the remembered record: {st, has, current}.
  function readState() {
    var st = hostState();
    var has = !!(st && st.record && st.record.version);
    var blocked = !!(withdrawalFailed || (st && st.withdrawalPending));
    var current = !!(!blocked && has && st.noticeVersion && st.record.version === st.noticeVersion);
    return { st: st, has: has, current: current, blocked: blocked };
  }

  // Paint the consent pill in the option row above the capture button. Plain-language states
  // only — the registry's notice version stays in the record, not the UI.
  function refreshStatus() {
    var pill = document.getElementById('btn-consent-status');
    if (!pill) return;
    var s = readState();
    if (s.blocked) {
      pill.textContent = '撤回处理中';
      pill.className = 'consent-pill state-warn';
      pill.title = '数据删除尚未确认完成，点击重试撤回';
    } else if (s.current) {
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

  // The status dialog always offers the notice viewer, plus withdrawal for
  // a current record or a way to reopen the consent gate otherwise.
  function statusMenu() {
    var s = readState();
    var opts = {
      title: '隐私同意',
      okText: '查看隐私说明',
      cancelText: '关闭',
      onOk: openPrivacyNotice
    };
    if (s.blocked) {
      opts.message = '撤回及数据删除尚未确认完成，或撤回状态无法读取。摄像头和录入保持关闭。请检查服务与权限后重试；此前的人脸登录可能仍可用。';
      opts.midText = '重试撤回';
      opts.onMid = withdraw;
    } else if (s.current || s.has) {
      opts.message = '你已同意《LockFace 人脸识别隐私说明与知情同意书》' +
                     '（' + fmtTime(s.st.record.time) + '）。摄像头与人脸采集在此同意下运行；' +
                     '撤回将删除当前账户全部人脸及保存的密码，并清理旧备份；再次使用需要重新录入。';
      if (!s.current) opts.message = '你此前同意的隐私说明已更新。可以直接撤回并删除已有数据；如需继续采集，请从录入页重新同意。' +
        '撤回将删除当前账户全部人脸及保存的密码，并清理旧备份。';
      opts.midText = '撤回同意';
      opts.onMid = withdraw;
    } else if (s.st) {
      opts.message = consentRequiredText + '点击“重新同意”可恢复预览。';
    } else {
      opts.message = '同意记录暂时读取不到；下次启动会重新征求同意。';
    }
    if (!s.blocked && !s.current && !s.has) {
      opts.midText = '重新同意';
      opts.onMid = function() {
        show(function() { previewCtl.restartPreview(); });
      };
    }
    ui.showAppModal(opts);
  }

  function decline() {
    var el = overlay();
    if (el) el.classList.remove('active');
    pending = null;
    ui.hideBanner('cam');
    showConsentRequired();
  }

  // Returns true when already granted. Otherwise shows the gate, remembers fn
  // as the resume action, and returns false so the caller stops immediately.
  function ensure(fn) {
    if (readState().blocked) {
      markWithdrawalPending();
      statusMenu();
      return false;
    }
    if (granted) return true;
    show(fn);
    return false;
  }

  function init(fn) {
    var box = agreeBox();
    if (box) box.addEventListener('change', syncButton);
    refreshStatus();
    if (readState().blocked) {
      markWithdrawalPending();
      ui.showBanner('cam', '上次撤回尚未确认完成。请点击“撤回处理中”重试；摄像头和录入保持关闭，此前的人脸登录可能仍可用。', 'error', 20000);
      return;
    }
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
    granted: function() { return granted; },
    guardCurrent: guardCurrent,
    showConsentRequired: showConsentRequired
  };
}
