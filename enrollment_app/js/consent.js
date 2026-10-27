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
  var consentRequiredText = lfT("privacy_consent_not_granted_the_camera_is");

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
    setCaption(lfT("starting_camera"));
    var loading = document.getElementById('cam-loading');
    if (!loading) return;
    loading.style.display = 'flex';
    loading.classList.remove('fade');
    loading.classList.remove('is-blocked');
    var spinner = loading.querySelector('.spinner-dark');
    if (spinner) spinner.style.display = '';
    var text = loading.querySelector('.loading-text');
    if (text) text.textContent = lfT("starting_camera");
  }

  function markCameraBlocked() {
    setCaption(consentRequiredText);
    var loading = document.getElementById('cam-loading');
    if (!loading) return;
    loading.style.display = 'flex';
    loading.classList.remove('fade');
    loading.classList.add('is-blocked');
    document.getElementById('camera-state-title').textContent = lfT('camera_off');
    var spinner = loading.querySelector('.spinner-dark');
    if (spinner) spinner.style.display = 'none';
    var text = loading.querySelector('.loading-text');
    if (text) text.textContent = lfT("camera_consent_hint");
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
        ui.showBanner('cam', lfT("unable_to_save_consent_finish_any_pending"), 'error', 15000);
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
      ui.showBanner('cam', lfT("consent_withdrawn_faces_saved_password_and_old"), 'ok', 12000);
    } else {
      markWithdrawalPending();
      ui.showBanner('cam', lfT("the_camera_is_off_but_withdrawal_and"), 'error', 20000);
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
    document.getElementById('camera-state-title').textContent = lfT('withdrawal_pending');
    if (text) text.textContent = lfT("withdrawal_pending_finish_deleting_data_before_giving");
    setCaption(lfT("withdrawal_pending_finish_deleting_data_before_giving"));
  }

  function fmtTime(t) {
    if (typeof t !== 'string' || t.length < 16) return lfT("unknown");
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
      pill.textContent = lfT("withdrawal_pending");
      pill.className = 'consent-pill state-warn';
      pill.title = lfT("deletion_is_not_yet_confirmed_click_to");
    } else if (s.current) {
      pill.textContent = lfT("privacy_consent_granted");
      pill.className = 'consent-pill state-ok';
      pill.title = lfT("you_consented_to_the_lockface_face_recognition");
    } else if (s.has) {
      pill.textContent = lfT("privacy_consent_needs_renewal");
      pill.className = 'consent-pill state-warn';
      pill.title = lfT("the_privacy_notice_has_changed_renew_consent");
    } else {
      pill.textContent = lfT("privacy_consent_not_granted");
      pill.className = 'consent-pill';
      pill.title = lfT("consent_is_required_to_start_the_camera");
    }
  }

  // The status dialog always offers the notice viewer, plus withdrawal for
  // a current record or a way to reopen the consent gate otherwise.
  function statusMenu() {
    var s = readState();
    var opts = {
      variant: 'consent',
      title: lfT("privacy_consent"),
      okText: lfT("view_privacy_notice"),
      cancelText: lfT("close"),
      onOk: openPrivacyNotice
    };
    if (s.blocked) {
      opts.message = lfT("withdrawal_and_deletion_are_unconfirmed_or_the");
      opts.midText = lfT("retry_withdrawal");
      opts.onMid = withdraw;
    } else if (s.current || s.has) {
      opts.message = lfT("consent_record_info", {time: fmtTime(s.st.record.time)});
      if (!s.current) opts.message = lfT("the_privacy_notice_you_previously_accepted_has") +
        lfT("withdrawal_deletes_all_faces_and_the_saved_2");
      opts.midText = lfT("withdraw_consent");
      opts.onMid = withdraw;
    } else if (s.st) {
      opts.message = consentRequiredText + '\n\n' + lfT("click_renew_consent_to_restore_the_preview");
    } else {
      opts.message = lfT("the_consent_record_is_temporarily_unavailable_you");
    }
    if (!s.blocked && !s.current && !s.has) {
      opts.midText = lfT("renew_consent");
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
      ui.showBanner('cam', lfT("the_previous_withdrawal_is_not_yet_confirmed"), 'error', 20000);
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
