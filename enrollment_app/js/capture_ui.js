// The capture state machine and the angle tables; other modules see only the
// readers below (navigation/app_main gate on isActive/canShowIdleTotal,
// enroll_save shows the appended angle's name). deps: {timers} — injected so
// tests can drive a sequence with mock timers (same as capture_flow).
function createCaptureCtl(deps) {
  var captureFlow = createCaptureFlow(deps.timers);
  var ANGLE_LABELS = ['正面', '左转', '右转'];
  var ANGLE_INSTR = {
    0: lfT("face_the_camera_and_hold_still"),
    1: lfT("capture_instruction_left"),
    2: lfT("capture_instruction_right")
  };
  return {
    isActive: function() { return captureFlow.isActive(); },
    canShowIdleTotal: function() { return captureFlow.canShowIdleTotal(); },
    isMultiAngle: function() { return captureFlow.isMultiAngle(); },
    firstAngle: function() { return captureFlow.firstAngle(); },
    angleLabel: function(i) { return lfStoredLabel(ANGLE_LABELS[i]); },
    instr: function(i) { return ANGLE_INSTR[i]; },
    begin: function(multi, firstAngle) { captureFlow.begin(multi, firstAngle); },
    end: function() { captureFlow.end(); },
    stopPolling: function() { captureFlow.stopPolling(); },
    poll: function(fn) { captureFlow.poll(fn); },
    scheduleNext: function(fn) { captureFlow.scheduleNext(fn); },
    // Scan the enrolled labels for the first angle slot not yet taken.
    firstMissingAngle: function(have) {
      for (var a = 0; a < ANGLE_LABELS.length; a++) {
        if (!have[ANGLE_LABELS[a]]) return a;
      }
      return 0;
    }
  };
}

// Cam-screen capture UI: button modes, progress, the per-angle poll loop and
// the post-capture branch to the password/append flows. deps: {host,
// showBanner, setCaption, captureCtl, switchTab, showScreen, previewCtl,
// showAppendScreen, showRefreshPrompt}.
function createCaptureUi(deps) {
  var H = deps.host;
  var showBanner = deps.showBanner, setCaption = deps.setCaption;
  var captureCtl = deps.captureCtl;
  var switchTab = deps.switchTab, showScreen = deps.showScreen;
  var previewCtl = deps.previewCtl;
  var showAppendScreen = deps.showAppendScreen;
  var showRefreshPrompt = deps.showRefreshPrompt;
  // Optional so the capture-machine tests can build this module standalone;
  // the real page always passes the consent gate's ensure().
  var requireConsent = deps.requireConsent || function() { return true; };

  // Button modes. Idle = primary「开始采集」; while capturing it becomes an
  // enabled outline「取消采集」— previously a started capture was locked in
  // until it finished or hit the 25s watchdog.
  function enterCaptureCancelMode() {
    var btn = document.getElementById('btn-capture');
    btn.disabled = false;
    btn.classList.remove('btn-primary');
    btn.classList.add('btn-secondary');
    document.getElementById('cap-btn-text').textContent = lfT("cancel_capture");
    document.getElementById('cap-spinner').style.display = 'none';
  }
  function resetCaptureUi() {
    captureCtl.end();
    var btn = document.getElementById('btn-capture');
    btn.disabled = false;
    btn.classList.add('btn-primary');
    btn.classList.remove('btn-secondary');
    document.getElementById('cap-btn-text').textContent = lfT("start_capture");
    document.getElementById('cap-spinner').style.display = 'none';
  }
  function hideCaptureProgress() {
    document.getElementById('cap-progress').style.display = 'none';
    document.getElementById('cap-fill').style.width = '0%';
  }

  // User pressed the cancel affordance mid-capture: stop the poll first (so no
  // later tick paints over the reset), then tell the host to stop the capture
  // thread cooperatively (both phases check m_capturing between frames). The
  // partial samples stay in host memory exactly like a failed capture does.
  function onCancelCapture() {
    captureCtl.end();
    try { H.CancelCapture(); } catch(e) {}
    resetCaptureUi();
    hideCaptureProgress();
    setCaption(lfT("capture_count", {count: 0}));
    showBanner('cam', lfT("capture_cancelled"), 'ok');
  }

  function onCapture(consentOverride) {
    // While a capture runs, the primary button doubles as its cancel control.
    if (captureCtl.isActive()) { onCancelCapture(); return; }

    // The first capture click in a session without consent opens the gate.
    // Acceptance resumes through restartPreview(), because declining left the
    // camera off; re-entry skips the gate exactly once.
    if (!consentOverride &&
        !requireConsent(function() {
          if (previewCtl.restartPreview()) onCapture(true);
        })) return;

    var prog = document.getElementById('cap-progress');

    // Pre-check face slot limit for single-angle mode.
    // Multi-angle always allowed (server replaces old faces via ClearFacesForAccount).
    var faceCount = 0;
    try { faceCount = H.GetFaceCount(); } catch(e) {}
    var multi = false;
    try { multi = document.getElementById('chk-multi-angle').checked; } catch(e) {}
    if (!multi && faceCount >= 3) {
      showBanner('cam', lfT("the_limit_is_3_faces_per_account"), 'error', 6000);
      return;
    }

    enterCaptureCancelMode();
    prog.style.display = 'block';
    document.getElementById('cap-fill').style.width = '0%';

    var angles = 1;
    if (multi) angles = 3;

    // Single-angle append: capture the FIRST MISSING angle, not always the
    // front. First enrollment (no faces) -> front angle.
    try {
      var firstAngle = multi ? 0 : pickMissingAngle(faceCount);
      captureCtl.begin(multi, firstAngle);
      captureAngleSequence(firstAngle, angles);
    } catch(e) {
      resetCaptureUi();
      hideCaptureProgress();
      showBanner('cam', lfT("unable_to_start_capture") + e.message, 'error');
    }
  }

  // For a single-angle append, pick the FIRST missing angle (正面/左转/右转)
  // based on the labels of faces already enrolled. Old records labeled 脸N
  // carry no angle info -> fall back to the front angle.
  function pickMissingAngle(faceCount) {
    if (faceCount <= 0) return 0;
    var have = {};
    try {
      var arr = JSON.parse(H.GetFacesJson());
      for (var i = 0; i < arr.length; i++) if (arr[i].label) have[arr[i].label] = true;
    } catch(e) {}
    return captureCtl.firstMissingAngle(have);
  }

  // Drive one capture angle at a time: call CaptureFaceSamples(angle), poll
  // GetCaptureStatus, show the instruction + live progress, then move on.
  function captureAngleSequence(idx, total) {
    if (!captureCtl.isActive()) return;

    // Start the capture. A false return means a previous capture is still
    // winding down server-side — surface that instead of freezing silently.
    var started = true;
    try { started = H.CaptureFaceSamples(idx); } catch(e) { started = false; }
    if (started === false) {
      resetCaptureUi();
      hideCaptureProgress();
      setCaption(lfT("waiting_for_the_previous_capture_to_end"));
      showBanner('cam', lfT("capture_could_not_start_the_previous_capture"), 'error');
      return;
    }

    // Reset the bar for the new angle: the completed-angle poll takes the
    // "done" branch and never repaints, so without this the previous angle's
    // last painted value (80% = 4/5 frames) stays frozen through the whole
    // liveness phase of this angle. Paint this angle's instruction right away
    // too — the previous angle's「完成」caption otherwise lingers until the
    // first poll (~200ms), which reads as a stuck bar.
    document.getElementById('cap-fill').style.width = '0%';
    setCaption((total > 1 ? lfT("capture_angle_prefix", {current: idx + 1, total: total}) : '')
               + (captureCtl.instr(idx) || lfT("capturing_face_samples")));

    // Watchdog: if the server reports no activity for 25s, the capture thread
    // is stuck (e.g. camera stall) — fail visibly instead of hanging forever.
    var pollStart = Date.now();
    var gotProgress = false;

    captureCtl.stopPolling();
    captureCtl.poll(function() {
      var st;
      try { st = JSON.parse(H.GetCaptureStatus()); } catch(e) { return; }

      // A cancel landed server-side while a poll was still in flight — the
      // cancel path has already reset the UI; just stand down.
      if (st.cancelled) {
        captureCtl.stopPolling();
        resetCaptureUi();
        hideCaptureProgress();
        return;
      }

      if (st.collected > 0) gotProgress = true;
      if (!gotProgress && !st.done && st.collected === 0 && Date.now() - pollStart > 25000) {
        captureCtl.stopPolling();
        resetCaptureUi();
        hideCaptureProgress();
        setCaption(lfT("capture_has_not_started"));
        showBanner('cam', lfT("capture_timed_out_before_starting_check_the"), 'error');
        return;
      }

      // Every angle has its own liveness phase; side-angle COM calls must not
      // inherit a stale proof from a previous capture.
      if (st.livenessChecking) {
        setCaption((total > 1 ? lfT("capture_angle_prefix", {current: idx + 1, total: total}) : '')
                   + lfT("checking_liveness"));
        return;
      }

      // Completion reasons distinguish missing input from an actual PAD rejection.
      if (st.done && !st.livenessPassed) {
        captureCtl.stopPolling();
        resetCaptureUi();
        hideCaptureProgress();
        setCaption(lfT("capture_failed"));
        var failures = {
          no_frames: lfT("the_camera_stopped_providing_new_frames_check"),
          no_face: lfT("no_face_detected_place_your_face_in"),
          inference_failed: lfT("face_model_inference_failed_check_the_model"),
          liveness_timeout: lfT("liveness_check_could_not_finish_keep_your"),
          liveness_failed: lfT("liveness_check_failed_improve_the_lighting_uncover"),
          sample_failed: lfT("face_capture_is_incomplete_adjust_your_pose")
        };
        showBanner('cam', failures[st.failureReason] || lfT("capture_failed_check_the_camera_lighting_and"), 'error', 10000);
        return;
      }

      // Still collecting this angle — show instruction + progress.
      if (st.collected < st.target) {
        setCaption((total > 1 ? lfT("capture_angle_prefix", {current: idx + 1, total: total}) : '')
                   + (captureCtl.instr(st.angle) || lfT("capturing")) + ' · '
                   + st.collected + '/' + st.target + lfT("frames"));
        var pct = Math.round(st.collected / st.target * 100);
        document.getElementById('cap-fill').style.width = pct + '%';
        return;
      }

      // This angle is complete — move on to the next one. Paint 100% AND a
      // matching caption: the step text used to keep saying「活体检测中」here,
      // which next to「角度 N · 完成」and a frozen 100% bar read as stuck.
      captureCtl.stopPolling();
      document.getElementById('cap-fill').style.width = '100%';
      setCaption(total > 1 ? lfT("capture_angle_complete", {current: idx + 1, total: total})
                           : lfT("capture_complete", {angle: captureCtl.angleLabel(idx)}));
      if (idx + 1 < total) {
        captureCtl.scheduleNext(function() { captureAngleSequence(idx + 1, total); });
        return;
      }

      finishCapture();
    });
  }

  // All angles captured — the original post-capture flow (append vs password).
  function finishCapture() {
    resetCaptureUi();
    hideCaptureProgress();
    setCaption(lfT("ready"));

    previewCtl.stopLoop();
    H.StopPreview();

    // 1.3.0: branch by whether the current account already has faces.
    // First enrollment -> password page; already enrolled -> append page
    // (no password needed — the session identity is the proof).
    var faceCount = 0;
    try { faceCount = H.GetFaceCount(); } catch(e) {}

    if (faceCount > 0) {
      // If the record is a stale account-type one (state 1: MSA→local,
      // state 2: local→MSA), block the append path — AddFace would keep
      // the stale password. Route to the refresh prompt instead.
      try {
        var st = JSON.parse(H.CheckAccountTypeChanged()).state;
        if (st === 1 || st === 2) {
          switchTab('faces');
          showRefreshPrompt(st);
          return;
        }
      } catch(e) {}
      showAppendScreen(faceCount, captureCtl.isMultiAngle());
      return;
    }

    showScreen('pwd');
    try { document.getElementById('fld-user').value = H.GetUsername(); } catch(e) {}
    try { document.getElementById('fld-acct-type').value = H.GetAccountType(); } catch(e) {}
    try {
      var sid = H.GetUserSid();
      document.getElementById('fld-sid').textContent = sid;
      document.getElementById('fld-sid-note').style.display = sid ? '' : 'none';
    } catch(e) {}
    try { document.getElementById('fld-face-label').value = ''; } catch(e) {}

    // Label the password field by account type: MSA accounts use the
    // Microsoft account password, local accounts use the Windows password.
    try {
      var acctType = document.getElementById('fld-acct-type').value;
      var passLabel = document.getElementById('fld-pass-label');
      var passInput = document.getElementById('fld-pass');
      if (acctType === 'msa') {
        passLabel.textContent = lfT("microsoft_account_password");
        passInput.placeholder = lfT("enter_your_microsoft_account_password");
      } else {
        passLabel.textContent = lfT("windows_password");
        passInput.placeholder = lfT("enter_your_sign_in_password");
      }
    } catch(e) {}

    document.getElementById('fld-pass').focus();
  }

  return { onCapture: onCapture, resetCaptureUi: resetCaptureUi };
}
