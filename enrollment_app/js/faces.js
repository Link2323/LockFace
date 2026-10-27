// ================================================================
// Multi-face management (1.3.0). deps: {host, showBanner, showAppModal,
// escapeHtml, switchTab, checkAccountRefresh} — the account refresh check
// runs on every load; the reverse direction (reload the list after a
// refresh) is injected the other way via accountRefresh.onChanged in the
// composition root, avoiding a construction cycle.
// ================================================================
function createFacesModule(deps) {
  var H = deps.host;
  var showBanner = deps.showBanner, showAppModal = deps.showAppModal, escapeHtml = deps.escapeHtml;
  var switchTab = deps.switchTab;
  var checkAccountRefresh = deps.checkAccountRefresh;

  function loadFaces() {
    checkAccountRefresh();  // prompt to refresh a stale MSA→local record if present
    try {
      var faces = JSON.parse(H.GetFacesJson());
      var count = H.GetFaceCount();
      var list = document.getElementById('face-list');
      document.getElementById('faces-count').textContent = lfT("faces_count", {count: count});
      try { document.getElementById('faces-user').textContent = H.GetUsername(); } catch(e) {}

      if (faces.length === 0) {
        list.innerHTML = "<div class=\"face-empty\">" + lfT("no_faces_enrolled_open_enroll_to_get") + "</div>";
        return;
      }
      var html = '';
      for (var i = 0; i < faces.length; i++) {
        var f = faces[i];
        html += '<div class="face-item">'
          + '<span class="face-name" title="' + escapeHtml(lfStoredLabel(f.label)) + '">' + escapeHtml(lfStoredLabel(f.label)) + '</span>'
          + '<span class="face-id">#' + f.id + '</span>'
          + '<button class="btn btn-secondary" onclick="onRenameFace(' + f.id + ")\">" + lfT("rename") + "</button>"
          + '<button class="btn btn-secondary" onclick="onDeleteFace(' + f.id + ")\">" + lfT("delete") + "</button>"
          + '</div>';
      }
      list.innerHTML = html;
    } catch(e) { showBanner('faces', lfT("unable_to_load_faces") + e.message, 'error'); }
  }

  function goAddFace() {
    var faceCount = 0;
    try { faceCount = H.GetFaceCount(); } catch(e) {}
    var multi = false;
    try { multi = document.getElementById('chk-multi-angle').checked; } catch(e) {}

    // Multi-angle re-enrollment REPLACES all old faces — destructive, so confirm
    // whenever the account already has faces (not only at the per-account cap).
    // Three explicit choices: replace, append a single angle, or back out —
    // "覆盖/追加都要继续" vs "什么都不做" must be separately clickable.
    if (multi && faceCount > 0) {
      showAppModal({
        title: lfT("replace_existing_faces"),
        message: lfT("faces_replace_info", {count: faceCount}),
        okText: lfT("replace_faces"),
        midText: lfT("add_one_angle"),
        onMid: function() {
          try { document.getElementById('chk-multi-angle').checked = false; } catch(e) {}
          switchTab('cam');
          showBanner('cam', lfT("click_start_capture_to_add_the_missing"), 'ok', 6000);
        },
        onOk: function() {
          switchTab('cam');
          showBanner('cam', lfT("multi_angle_enrollment_replaces_all_old_faces"), 'ok', 6000);
        }
        // 取消: plain dismiss — no side effect, user stays on the faces screen.
      });
      return;
    }

    // Single-angle append at the per-account cap is impossible — force
    // multi-angle replacement instead.
    if (faceCount >= 3) {
      try { document.getElementById('chk-multi-angle').checked = true; } catch(e) {}
      showAppModal({
        title: lfT("replace_existing_faces"),
        message: lfT("this_account_has_reached_the_limit_of"),
        okText: lfT("replace_faces"),
        onOk: function() {
          switchTab('cam');
          showBanner('cam', lfT("multi_angle_enrollment_replaces_all_old_faces"), 'ok', 6000);
        }
      });
      return;
    }

    switchTab('cam');
    showBanner('cam', lfT("click_start_capture_to_add_a_face"), 'ok', 6000);
  }

  function onDeleteFace(id) {
    showAppModal({
      title: lfT("delete_this_face"),
      message: lfT("this_cannot_be_undone"),
      okText: lfT("delete_2"),
      onOk: function() {
        try {
          finishMutation(H.DeleteFace(id), lfT("deleted"), lfT("unable_to_delete_check_the_logs_and"));
        } catch(e) { showBanner('faces', lfT("unable_to_delete") + e.message, 'error'); }
      }
    });
  }

  function onRenameFace(id) {
    showAppModal({
      title: lfT("name_this_face"),
      input: true,
      okText: lfT("save"),
      onOk: function(name) {
        name = (name || '').trim();
        try {
          finishMutation(H.RenameFace(id, name), lfT("renamed"), lfT("unable_to_rename_check_the_logs_and"));
        } catch(e) { showBanner('faces', lfT("unable_to_rename") + e.message, 'error'); }
      }
    });
  }

  function onClearAllFaces() {
    showAppModal({
      title: lfT("clear_all_faces"),
      message: lfT("face_sign_in_will_no_longer_be"),
      okText: lfT("clear"),
      onOk: function() {
        try {
          finishMutation(H.ClearAllFaces(), lfT("all_faces_cleared"), lfT("unable_to_clear_faces_check_the_logs"));
        } catch(e) { showBanner('faces', lfT("unable_to_clear_faces") + e.message, 'error'); }
      }
    });
  }

  function finishMutation(ok, success, failure) {
    if (ok) {
      loadFaces();
      showBanner('faces', success, 'ok');
      return;
    }
    var persisted = false;
    try { persisted = H.GetAccountSaveStatus() === 'reload_failed'; } catch(e) {}
    if (persisted) {
      loadFaces();
      showBanner('faces', lfT("changes_saved_and_the_list_updated_but"), 'error', 15000);
    } else {
      showBanner('faces', failure, 'error', 10000);
    }
  }

  return {
    load: loadFaces,
    goAddFace: goAddFace,
    onDeleteFace: onDeleteFace,
    onRenameFace: onRenameFace,
    onClearAllFaces: onClearAllFaces
  };
}
