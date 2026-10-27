// Post-capture flows: the append-face screen (account already has faces) and
// the first-enrollment password screen. deps: {host, showBanner, setCaption,
// withBusy, captureCtl, showScreen, previewCtl}.
function createEnrollSaveModule(deps) {
  var H = deps.host;
  var showBanner = deps.showBanner, setCaption = deps.setCaption;
  var withBusy = deps.withBusy;
  var captureCtl = deps.captureCtl;
  var showScreen = deps.showScreen;
  var previewCtl = deps.previewCtl;

  function savedWithoutReload() {
    try { return H.GetEnrollmentSaveStatus() === 'reload_failed'; } catch(e) { return false; }
  }

  function showSavedWithoutReload() {
    showScreen('cam');
    document.getElementById('fld-pass').value = '';
    setCaption(lfT("face_saved"));
    previewCtl.restartPreview();
    showBanner('cam', lfT("your_face_was_saved_but_the_service"), 'error', 15000);
  }

  // Show the append-face confirmation screen (current account already has
  // faces). When isReplace is true (multi-angle re-enrollment), old faces are
  // cleared and the three new angle faces replace them. Otherwise it is a
  // single-angle append.
  function showAppendScreen(faceCount, isReplace) {
    showScreen('append');
    try { document.getElementById('append-user').value = H.GetUsername(); } catch(e) {}
    try {
      var appendSid = H.GetUserSid();
      document.getElementById('append-sid').textContent = appendSid;
      document.getElementById('append-sid-note').style.display = appendSid ? '' : 'none';
    } catch(e) {}
    try { document.getElementById('append-face-label').value = ''; } catch(e) {}

    if (isReplace) {
      document.getElementById('append-title').textContent = lfT("replace_all_faces");
      document.getElementById('append-label-field').style.display = 'none';
      document.getElementById('append-info').textContent =
        lfT("enroll_replace_info", {count: faceCount});
      document.getElementById('btn-append-text').textContent = lfT("confirm_replacement");
      showBanner('append', lfT("multi_angle_enrollment_old_faces_will_be"), 'ok');
    } else {
      document.getElementById('append-title').textContent = lfT("add_a_face");
      document.getElementById('append-label-field').style.display = '';
      document.getElementById('append-info').textContent =
        lfT("enroll_append_info", {count: faceCount, angle: captureCtl.angleLabel(captureCtl.firstAngle())});
      document.getElementById('btn-append-text').textContent = lfT("confirm_addition");
    }
  }

  function onAppendConfirm() {
    var label = '';
    try { label = document.getElementById('append-face-label').value.trim(); } catch(e) {}
    withBusy([document.getElementById('btn-append'),
              document.getElementById('btn-append-cancel')],
             document.getElementById('append-spinner'),
    function() {
      var ok = false;
      try { ok = H.SaveEnrollmentAppend(label); } catch(e) { ok = false; }

      if (ok) {
        showScreen('cam');
        setCaption(lfT("ready"));
        var okMsg = lfT("face_added_press_win_l_to_lock");
        showBanner('cam', okMsg, 'ok');
        previewCtl.restartPreview();
      } else if (savedWithoutReload()) {
        showSavedWithoutReload();
      } else {
        showBanner('append', lfT("unable_to_save_face_check_the_service"), 'error', 10000);
      }
    });
  }

  function onAppendCancel() {
    showScreen('cam');
    // The captured samples were discarded by this cancel — never show the old
    //「已采 N 帧」total again.
    setCaption(lfT("capture_count", {count: 0}));
    previewCtl.restartPreview();
  }

  // Cancel from the password screen: discard the just-captured samples and
  // return to the camera screen, restarting the live preview. Mirrors the
  // cleanup done in onSave's success path.
  function onPwdCancel() {
    showScreen('cam');
    document.getElementById('fld-pass').value = '';
    setCaption(lfT("capture_count", {count: 0}));
    previewCtl.restartPreview();
  }

  function onSave() {
    var pass = document.getElementById('fld-pass').value;
    if (!pass) { showBanner('pwd', lfT("enter_your_password"), 'error'); return; }

    withBusy([document.getElementById('btn-save'), document.getElementById('btn-save-cancel')],
             document.getElementById('save-spinner'),
    function() {
      var passwordValid = false, validationFailed = false;
      var label = '';
      try { label = document.getElementById('fld-face-label').value.trim(); } catch(e) {}
      try { passwordValid = H.ValidatePassword(pass); } catch(e) { validationFailed = true; }
      if (validationFailed) {
        showBanner('pwd', lfT("unable_to_complete_password_verification_try_again"), 'error', 10000);
      } else if (passwordValid) {
        var ok = false;
        try { ok = H.SaveEnrollment(pass, label); } catch(e) {}
        if (!ok) {
          if (savedWithoutReload()) showSavedWithoutReload();
          else showBanner('pwd', lfT("password_verified_but_saving_the_face_failed"), 'error', 10000);
          return;
        }
        // Return to the camera (main) screen immediately
        showScreen('cam');
        document.getElementById('fld-pass').value = '';
        setCaption(lfT("ready"));
        var okMsg = lfT("enrollment_complete_press_win_l_to_lock");
        showBanner('cam', okMsg, 'ok');
        previewCtl.restartPreview();
      } else {
        // MSA password validation only checks the locally cached credential.
        // If the user changed their Microsoft password elsewhere, the cache is
        // stale and the CURRENT (correct) password fails here. Guide them to
        // unlock once with the new password on the lock screen, which refreshes
        // the cache. Local accounts validate reliably offline.
        var acctType = '';
        try { acctType = document.getElementById('fld-acct-type').value; } catch(e) {}
        if (acctType === 'msa') {
          showBanner('pwd',
            lfT("password_verification_failed_if_you_recently_changed"),
            'error', 15000);
        } else {
          showBanner('pwd', lfT("incorrect_password_enter_your_windows_sign_in"), 'error');
        }
      }
    });
  }

  return {
    showAppendScreen: showAppendScreen,
    onAppendConfirm: onAppendConfirm,
    onAppendCancel: onAppendCancel,
    onPwdCancel: onPwdCancel,
    onSave: onSave
  };
}
