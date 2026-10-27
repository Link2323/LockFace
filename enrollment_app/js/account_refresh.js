// ================================================================
// Stale account-type refresh (1.4.0)
// Detect a record whose identity no longer matches the current account type
// after an MSA↔local conversion (Windows keeps the same SID, so the record is
// matched but carries stale identity/password):
//   state 1 = MSA→local: record UPN is an old Microsoft email.
//   state 2 = local→MSA: record UPN empty/different, current session is MSA.
// Offer to refresh: preserve faces, rewrite the UPN to the current account
// type, re-capture the current password. Detection re-evaluated on every
// faces-screen entry and at startup, so dismissing just hides the prompt.
// deps: {host, showBanner, withBusy}. The faces-list reload after a refresh
// or cleanup is NOT a dep — faces depends on this module for the entry check,
// so the reload is injected the other way via onChanged (registered in the
// composition root).
// ================================================================
function createAccountRefreshModule(deps) {
  var H = deps.host;
  var showBanner = deps.showBanner;
  var withBusy = deps.withBusy;

  // Account-type-change state shown in the refresh modal (0/1/2).
  var refreshState = 0;
  var reloadListeners = [];

  function notifyReload() {
    for (var i = 0; i < reloadListeners.length; i++) reloadListeners[i]();
  }

  function checkAccountRefresh() {
    var s = 0, faces = 0, upn = '';
    try {
      var j = JSON.parse(H.CheckAccountTypeChanged());
      s = j.state;
      faces = j.faces || 0;
      upn = j.upn || '';
    } catch(e) { return; }
    if (s === 1 || s === 2) showRefreshPrompt(s, faces, upn);
  }

  function showRefreshPrompt(state, faces, upn) {
    refreshState = state;
    var overlay = document.getElementById('refresh-overlay');
    overlay.dataset.refreshState = String(state);  // snapshot at open time
    var info = document.getElementById('refresh-info');
    var title = document.getElementById('refresh-title');
    var dismissBtn = document.getElementById('refresh-dismiss');
    var facesText = (faces > 0) ? lfT("account_enrolled_count", {count: faces}) : '';
    if (state === 2) {
      // local→MSA: record is local-era, now signed in with a Microsoft account.
      title.textContent = lfT("account_identity_changed");
      info.textContent = facesText
        + lfT("account_msa_prompt")
        + (upn ? lfT("account_msa_binding", {upn: upn}) : "")
        + lfT("enter_your_current_microsoft_account_password");
      dismissBtn.textContent = lfT("not_now");
    } else {
      // MSA→local: record still carries the old Microsoft email.
      title.textContent = lfT("account_identity_changed");
      info.textContent = facesText
        + lfT("account_local_prompt");
      dismissBtn.textContent = lfT("clear_account_email_only");
    }
    document.getElementById('refresh-pass').value = '';
    overlay.classList.add('active');
    setTimeout(function() { document.getElementById('refresh-pass').focus(); }, 50);
  }

  function onRefreshConfirm() {
    var pass = document.getElementById('refresh-pass').value;
    var info = document.getElementById('refresh-info');
    if (!pass) { info.textContent = lfT("enter_your_current_sign_in_password"); return; }

    withBusy([document.getElementById('refresh-confirm'),
              document.getElementById('refresh-dismiss')],
             document.getElementById('refresh-spinner'),
    function() {
      var ok = false;
      try { ok = H.RefreshAccountIdentity(pass); } catch(e) { ok = false; }
      document.getElementById('refresh-pass').value = '';
      if (ok) {
        document.getElementById('refresh-overlay').classList.remove('active');
        showBanner('faces', lfT("account_details_refreshed_your_faces_have_been"), 'ok', 6000);
        notifyReload();  // faces-user now shows the current account name
      } else {
        info.textContent =
          lfT("password_verification_or_account_refresh_failed_check");
        setTimeout(function() { document.getElementById('refresh-pass').focus(); }, 50);
      }
    });
  }

  function onRefreshDismiss() {
    withBusy([document.getElementById('refresh-dismiss')], null, function() {
      document.getElementById('refresh-overlay').classList.remove('active');
      if (refreshState !== 1) {
        // state 2 (local→MSA) needs the refresh to write the current email and
        // re-encrypt the password — there is nothing safe to auto-clean.
        showBanner('faces', lfT("skipped_for_now_you_will_be_prompted"), 'ok', 6000);
        return;
      }
      // state 1 (MSA→local): clear the stale MSA email so the lock-screen stops
      // packing the misattributed identity — faces and the stored password are
      // preserved. The full "刷新" path remains the right choice if the Windows
      // password changed since enrollment.
      var ok = false;
      try { ok = H.ClearStaleAccountUpn(); } catch(e) { ok = false; }
      if (ok) {
        showBanner('faces', lfT("old_account_email_cleared_your_faces_have"), 'ok', 6000);
        notifyReload();
      } else {
        showBanner('faces', lfT("cleanup_incomplete_choose_refresh_and_keep_faces"), 'error', 6000);
      }
    });
  }

  return {
    check: checkAccountRefresh,
    showRefreshPrompt: showRefreshPrompt,
    onRefreshConfirm: onRefreshConfirm,
    onRefreshDismiss: onRefreshDismiss,
    onChanged: function(fn) { reloadListeners.push(fn); }
  };
}
