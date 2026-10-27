// Log viewer + the learning status view. deps: {host, escapeHtml}. Learning
// view throttles to one pipe query per 10s: the public pipe is
// single-instance, so the 2s log tick must not become a 2s poll on it. The
// last-source memory forces one immediate repaint when the dropdown switches
// views. Both are private to this module.
function createLogsModule(deps) {
  var H = deps.host;
  var escapeHtml = deps.escapeHtml;

  var learningLastFetch = 0;
  var lastLogSource = '';
  function learningFetchDue(now) { return now - learningLastFetch >= 10000; }
  function markLearningFetch(now) { learningLastFetch = now; }
  function logSourceChanged(src) {
    var changed = (src !== lastLogSource);
    lastLogSource = src;
    return changed;
  }

  // Console-friendly labels for the learner's skip-reason keys (server-side
  // keys are stable ASCII; unknown keys fall through verbatim).
  var LEARN_REASONS = {
    distance: lfT("distance_gate"), clarity: lfT("identity_margin"), cone: lfT("pose_cone"),
    norm: lfT("norm_floor"), interval: lfT("update_interval"), disabled: lfT("learning_disabled"),
    vanished: lfT("template_removed"), error: lfT("other_error")
  };

  function renderLearningStatus(force) {
    var now = Date.now();
    if (!force && !learningFetchDue(now)) return;
    markLearningFetch(now);
    var viewer = document.getElementById('log-viewer');
    var html;
    try {
      var st = JSON.parse(H.GetLearningStatus());
      if (st.error) {
        html = "<div class=\"log-warn\">" + lfT("learning_status_unavailable") + escapeHtml(st.error) +
               lfT("the_service_must_be_running") + "</div>";
      } else {
        html = "<div class=\"log-info\">" + lfT("template_learning") + (st.enabled ? lfT("enabled") : lfT("disabled")) +
               lfT("distance_cap") + Number(st.gate_cap).toFixed(2) +
               ' · α=' + Number(st.alpha).toFixed(2) +
               lfT("interval") + st.interval + lfT("s_refreshes_every_10_seconds") + "</div>";
        html += "<div class=\"log-debug\">" + lfT("per_face_statistics") + "</div>";
        if (!st.faces || !st.faces.length) {
          html += "<div class=\"log-debug\">" + lfT("no_learning_records_yet_statistics_appear_after") + "</div>";
        } else {
          for (var i = 0; i < st.faces.length; i++) {
            var f = st.faces[i];
            var p20 = (f.era_p20 >= 0)
              ? lfT("epoch_p20") + Number(f.era_p20).toFixed(3) + '（' + f.era_rounds + lfT("rounds_independent_per_slot")
              : lfT("not_enough_epoch_samples") + f.era_rounds + lfT("5_rounds_using_account_pool_cap");
            html += '<div class="log-info">' + escapeHtml(f.user || '?') + lfT("face") + f.face +
                    '(' + escapeHtml(f.label || '?') + lfT("total") + f.total + lfT("today") + f.today +
                    lfT("last_accepted") + (f.last || lfT("never")) + ' · ' + p20 + '</div>';
          }
        }
        html += "<div class=\"log-debug\">" + lfT("recent_events_newest_first_up_to_24") + "</div>";
        if (!st.recent || !st.recent.length) {
          html += "<div class=\"log-debug\">" + lfT("no_events") + "</div>";
        } else {
          for (var j = 0; j < st.recent.length; j++) {
            var ev = st.recent[j];
            var who = escapeHtml(ev.user || '?') + '/' + escapeHtml(ev.label || '?');
            if (ev.ok) {
              html += '<div class="log-info">' + ev.t + lfT("ok_updated_d") +
                      Number(ev.d).toFixed(3) + ' α=' + Number(ev.alpha).toFixed(3) +
                      '  ' + who + '</div>';
            } else {
              var reason = LEARN_REASONS[ev.reason] || ev.reason;
              var detail = (ev.reason === 'distance')
                ? 'd=' + Number(ev.d).toFixed(3) + lfT("gate") + Number(ev.gate).toFixed(3)
                : 'd=' + Number(ev.d).toFixed(3);
              html += '<div class="log-warn">' + ev.t + lfT("skipped") + escapeHtml(reason) +
                      '  ' + detail + '  ' + who + '</div>';
            }
          }
        }
      }
    } catch(e) {
      html = "<div class=\"log-error\">" + lfT("unable_to_read_learning_status") + escapeHtml(e.message) + '</div>';
    }
    viewer.innerHTML = html;
  }

  function refreshLogs() {
    try {
      var viewer = document.getElementById('log-viewer');
      // The 2s auto-refresh repaints innerHTML, which destroys any in-progress
      // text selection — that made it impossible to select a log line to copy.
      // Skip this tick while a selection inside the viewer is active; refreshing
      // resumes on the next tick after the user clicks away.
      var selection = window.getSelection();
      if (selection && !selection.isCollapsed && viewer.contains(selection.anchorNode)) return;
      var src = document.getElementById('log-source').value;
      var force = logSourceChanged(src);
      if (src === 'learning') {
        // No file to clear behind this view — hide the button for it.
        document.getElementById('btn-clear-log').style.display = 'none';
        renderLearningStatus(force);
        return;
      }
      document.getElementById('btn-clear-log').style.display = '';
      var json;
      if (src === 'service') {
        json = H.GetServiceLogLines();
      } else if (src === 'worker') {
        json = H.GetAuthWorkerLogLines();
      } else if (src === 'cp') {
        json = H.GetCredentialProviderLogLines();
      } else {
        json = H.GetLogLines();
      }
      if (!json || json.length < 2 || json[0] !== '[') {
        document.getElementById('log-viewer').innerHTML = '<div class="log-warn">No log data</div>';
        return;
      }
      var lines = JSON.parse(json);
      var html = '';
      for (var i = 0; i < lines.length; i++) {
        var cls = 'log-info';
        if (lines[i].indexOf('[DEBUG]') >= 0) cls = 'log-debug';
        else if (lines[i].indexOf('[WARN]') >= 0) cls = 'log-warn';
        else if (lines[i].indexOf('[ERROR]') >= 0) cls = 'log-error';
        html += '<div class="' + cls + '">' + escapeHtml(lines[i]) + '</div>';
      }
      var atBottom = (viewer.scrollHeight - viewer.scrollTop - viewer.clientHeight) < 60;
      viewer.innerHTML = html;
      if (atBottom) viewer.scrollTop = viewer.scrollHeight;
    } catch(e) {
      document.getElementById('log-viewer').innerHTML = '<div class="log-error">Error loading logs: ' + e.message + '</div>';
    }
  }

  // 四个来源行为一致:清空 = 截断当日日志文件(console 同时清内存环形缓冲)。
  // writer 以追加模式(FILE_APPEND_DATA)打开文件,截断安全,下一条日志自动落在新末尾;
  // 按天轮转的历史文件不受影响。
  function clearLogs() {
    var src = document.getElementById('log-source').value;
    if (src === 'learning') return;  // live view, nothing to clear
    var ok = false;
    try { ok = src === 'console' ? H.ClearLog() : H.ClearFileLog(src); } catch(e) {}
    if (ok) document.getElementById('log-viewer').textContent = '';
    var btn = document.getElementById('btn-clear-log');
    btn.textContent = ok ? lfT("cleared") : lfT("unable_to_clear_check_log_permissions");
    setTimeout(function() { btn.textContent = lfT("clear_logs"); }, ok ? 1500 : 6000);
  }

  // Copy exactly what the viewer currently shows (the selected source). The app
  // is loaded via NavigateToString (about:blank origin), where the async
  // Clipboard API may be unavailable — fall back to execCommand.
  function copyLogs() {
    var text = document.getElementById('log-viewer').innerText;
    if (!text) return;
    var btn = document.getElementById('btn-copy-log');
    var done = function() {
      btn.textContent = lfT("copied");
      setTimeout(function() { btn.textContent = lfT("copy"); }, 1500);
    };
    var fallback = function() {
      var ta = document.createElement('textarea');
      ta.value = text;
      ta.style.position = 'fixed';
      ta.style.opacity = '0';
      document.body.appendChild(ta);
      ta.select();
      try { if (document.execCommand('copy')) done(); } catch(e) {}
      document.body.removeChild(ta);
    };
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(done, fallback);
    } else {
      fallback();
    }
  }

  return {
    refresh: refreshLogs,
    clearLogs: clearLogs,
    copyLogs: copyLogs
  };
}
