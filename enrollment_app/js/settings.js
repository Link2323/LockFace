// Settings screen + first-run camera picker. Shared state: the last config
// loaded by loadSettings (kept whole for onSaveSettings) and the selected
// unlock tier index. deps: {host, showBanner, previewCtl, syncSelects}.
function createSettingsModule(deps) {
  var H = deps.host;
  var showBanner = deps.showBanner;
  var previewCtl = deps.previewCtl;
  // Re-syncs the custom-select facades after this module re-points a hidden
  // select programmatically (.value writes fire no events).
  var syncSelects = deps.syncSelects || function() {};

  var loadedConfig = null;
  var saving = false;

  // The host applies settings on its UI thread and starts a bounded service
  // request in the background. Poll only its in-memory status, never the pipe.
  // Both entry points share one gate so a picker save cannot race a tab save.
  function saveConfig(cfg, done) {
    if (saving) return;
    saving = true;
    var ids = ['btn-save-settings', 'btn-reset-settings', 'cam-picker-confirm',
               'cam-picker-skip', 'cam-picker-select'];
    var controls = ids.map(function(id) { return document.getElementById(id); });
    controls.forEach(function(el) { if (el) el.disabled = true; });
    syncSelects();
    var spinner = document.getElementById('settings-spinner');
    spinner.style.display = 'inline-block';

    function finish(status) {
      saving = false;
      controls.forEach(function(el) { if (el) el.disabled = false; });
      syncSelects();
      spinner.style.display = 'none';
      done(status);
    }
    function poll() {
      var status;
      try { status = H.GetConfigSaveStatus(); }
      catch(e) { finish('unknown'); return; }
      if (status === 'pending') { setTimeout(poll, 150); return; }
      finish(status);
    }
    setTimeout(function() {
      try {
        if (!H.BeginConfigSave(JSON.stringify(cfg))) { finish('save_failed'); return; }
      } catch(e) { finish('unknown'); return; }
      poll();
    }, 50);
  }

  function configSaveError(status) {
    if (status === 'reload_failed') {
      return '设置已保存，但后台服务未确认安全重载；请检查服务状态和模型文件后重试。';
    }
    if (status === 'save_failed') return '设置保存失败，请检查日志后重试。';
    return '无法确认设置保存结果，请检查服务状态和日志后重试。';
  }

  function pickerError(text) {
    var el = document.getElementById('cam-picker-error');
    el.textContent = text;
    el.style.display = text ? '' : 'none';
  }

  // Unlock tier selector detents, strict→fast (left→right). The three radios
  // (#cfg-unlockframes-N) carry the visual state — the sliding pill is pure
  // CSS on :checked — while this index is what onSaveSettings writes; the
  // wire format stays unlock_frames {5,3,2}. Anything unexpected from config
  // displays as the standard 3.
  var UNLOCK_TIERS = [
    {frames: 5, name: '严格'},
    {frames: 3, name: '标准'},
    {frames: 2, name: '极速'}
  ];
  var curTierIdx = 1;

  function unlockTierIndex(frames) {
    for (var i = 0; i < UNLOCK_TIERS.length; i++) {
      if (UNLOCK_TIERS[i].frames === frames) return i;
    }
    return 1;
  }

  function setTierChecked(idx) {
    for (var i = 0; i < UNLOCK_TIERS.length; i++) {
      var el = document.getElementById('cfg-unlockframes-' + i);
      if (el) el.checked = (i === idx);
    }
  }

  // Called from each radio's inline onchange with its value.
  function onUnlockTierInput(value) {
    var idx = Number(value);
    if (idx >= 0 && idx < UNLOCK_TIERS.length) curTierIdx = idx;
  }

  function loadSettings() {
    try {
      var json = H.GetConfig();
      var cfg = JSON.parse(json);
      // Keep the full parsed config for onSaveSettings: the backend's BeginConfigSave
      // parser starts from defaults, so saving only the UI-managed keys would
      // silently reset non-UI fields (learning gates, alpha, interval, ...).
      loadedConfig = cfg;
      document.getElementById('cfg-rotation').value = String(cfg.camera_rotation || 0);
      document.getElementById('cfg-learning').checked = (cfg.ema_learning !== false);
      // Unlock tier {5,3,2}; anything unexpected displays as the standard 3.
      var unlockFrames = Number(cfg.unlock_frames || 3);
      if (unlockFrames !== 5 && unlockFrames !== 2) unlockFrames = 3;
      curTierIdx = unlockTierIndex(unlockFrames);
      setTierChecked(curTierIdx);

      // Populate the camera dropdown from the enumerated devices.
      var camSel = document.getElementById('cfg-camera');
      var currentCam = cfg.camera_device || '';
      // Keep the "default" option as first.
      camSel.innerHTML = '<option value="">默认（第一个设备）</option>';
      try {
        var camList = JSON.parse(H.GetCameraList());
        var found = false;
        for (var i = 0; i < camList.length; i++) {
          var opt = document.createElement('option');
          opt.value = camList[i].path;
          opt.textContent = camList[i].name || ('摄像头 ' + (i + 1));
          camSel.appendChild(opt);
          if (currentCam && camList[i].path === currentCam) found = true;
        }
        if (found) {
          camSel.value = currentCam;
        } else if (currentCam) {
          // Configured camera is no longer present — keep it visible so the
          // user knows; the backend refuses to substitute another device.
          var missingOpt = document.createElement('option');
          missingOpt.value = currentCam;
          missingOpt.textContent = '(未找到: ' + currentCam + ')';
          camSel.appendChild(missingOpt);
          camSel.value = currentCam;
        }
      } catch(e) {}

      // match_threshold is now honored in the calibrated band [0.70, 1.00]
      // (EmbeddingThresholdForDim clamps outside values). Slider maps directly
      // to the distance threshold; lower = stricter.
      var matchThr = Number(cfg.match_threshold || 1.00);
      matchThr = Math.max(0.70, Math.min(1.00, matchThr));   // clamp to slider range
      document.getElementById('cfg-threshold').value = matchThr;
      document.getElementById('cfg-threshold-val').textContent = matchThr.toFixed(2);
      // Snap onto the slider's 0.01 grid: configs written by older builds may
      // carry values like 0.281 that the step-0.01 slider can no longer hold.
      var asThr = Number(cfg.anti_spoof_threshold || 0.28);
      asThr = Math.round((asThr - 0.15) / 0.01) * 0.01 + 0.15;
      if (asThr < 0.15) asThr = 0.15;
      if (asThr > 0.50) asThr = 0.50;
      document.getElementById('cfg-antispoof-threshold').value = asThr;
      document.getElementById('cfg-asthr-val').textContent = asThr.toFixed(2);
      // Learning gate cap: slider grid [0.60, 0.68]; hand-edited configs can be
      // stricter (down to 0.35) — the loader honors that, the slider snaps to
      // its own range on display (a settings save then writes the snapped
      // value; the effective gate is min(user p20, cap) so this stays strict).
      var lgThr = Number(cfg.learning_distance_gate || 0.65);
      lgThr = Math.round((lgThr - 0.60) / 0.01) * 0.01 + 0.60;
      if (lgThr < 0.60) lgThr = 0.60;
      if (lgThr > 0.68) lgThr = 0.68;
      document.getElementById('cfg-learn-gate').value = lgThr;
      document.getElementById('cfg-learn-gate-val').textContent = lgThr.toFixed(2);
      syncSelects();
    } catch(e) { showBanner('settings', '读取设置失败: ' + e.message, 'error'); }
  }

  function onSaveSettings() {
    if (saving) return;
    // Single UI switch drives both learning channels so "off" means templates
    // are strictly read-only (docs/work/in-progress/progressive-learning-v2.md red line 5).
    var learning = document.getElementById('cfg-learning').checked;
    // The tier index tracks the radios via onchange; guard anyway so an out-
    // of-range state can never write a bogus unlock_frames.
    var tierIdx = (curTierIdx >= 0 && curTierIdx < UNLOCK_TIERS.length) ? curTierIdx : 1;
    // Round-trip the FULL config: UI-managed keys overridden, every other key
    // preserved from the last load (see loadSettings note on partial saves).
    var cfg = Object.assign({}, loadedConfig || {}, {
      match_threshold: parseFloat(document.getElementById('cfg-threshold').value),
      anti_spoof_threshold: parseFloat(document.getElementById('cfg-antispoof-threshold').value),
      camera_rotation: parseInt(document.getElementById('cfg-rotation').value) || 0,
      camera_device: document.getElementById('cfg-camera').value,
      ema_learning: learning,
      failure_learning: learning,
      unlock_frames: UNLOCK_TIERS[tierIdx].frames,
      learning_distance_gate: parseFloat(document.getElementById('cfg-learn-gate').value)
    });
    showBanner('settings', '正在保存设置并等待后台服务确认…', 'ok', 30000);
    saveConfig(cfg, function(status) {
      if (status === 'saved') {
        showBanner('settings', '设置已保存并通知后台服务。', 'ok');
      } else {
        showBanner('settings', configSaveError(status), 'error', 15000);
      }
    });
  }

  // Fill the form with display-side defaults (mirrors the backend's
  // ConfigFromJson starting point). Nothing is written until the user clicks
  // 保存设置, which round-trips the loaded config for the non-UI keys.
  function onResetDefaults() {
    if (saving) return;
    document.getElementById('cfg-rotation').value = '0';
    var camSel = document.getElementById('cfg-camera');
    if (camSel.querySelector('option[value=""]')) camSel.value = '';
    document.getElementById('cfg-learning').checked = false;
    curTierIdx = 1;
    setTierChecked(curTierIdx);
    document.getElementById('cfg-threshold').value = 1.00;
    document.getElementById('cfg-threshold-val').textContent = '1.00';
    document.getElementById('cfg-antispoof-threshold').value = 0.28;
    document.getElementById('cfg-asthr-val').textContent = '0.28';
    document.getElementById('cfg-learn-gate').value = 0.65;
    document.getElementById('cfg-learn-gate-val').textContent = '0.65';
    syncSelects();
    showBanner('settings', '已恢复默认值，点击“保存设置”后生效。', 'ok');
  }

  // ==================================================================
  // First-run camera picker: if config has no camera_device (fresh install
  // or the user never touched settings) AND more than one camera is present,
  // prompt the user to pick one. Saves via BeginConfigSave so subsequent launches
  // and the service use the chosen device.
  // ==================================================================
  function showCamPicker() {
    pickerError('');
    var sel = document.getElementById('cam-picker-select');
    sel.innerHTML = '';
    var camList = [];
    try { camList = JSON.parse(H.GetCameraList()); } catch(e) {}
    if (camList.length < 2) return;  // nothing to choose from / no multi-cam
    for (var i = 0; i < camList.length; i++) {
      var opt = document.createElement('option');
      opt.value = camList[i].path;
      opt.textContent = camList[i].name || ('摄像头 ' + (i + 1));
      sel.appendChild(opt);
    }
    document.getElementById('cam-picker-overlay').classList.add('active');
  }

  function onCamPickerConfirm() {
    if (saving) return;
    var sel = document.getElementById('cam-picker-select');
    var path = sel.value;
    if (!path) return;
    pickerError('');
    var cfg;
    try { cfg = JSON.parse(H.GetConfig()); }
    catch(e) { pickerError('无法读取摄像头设置，请检查日志后重试。'); return; }
    cfg.camera_device = path;
    document.getElementById('cam-picker-confirm').textContent = '正在保存，请稍候…';
    saveConfig(cfg, function(status) {
      document.getElementById('cam-picker-confirm').textContent = '使用此摄像头';
      if (status === 'saved') {
        document.getElementById('cam-picker-overlay').classList.remove('active');
        showBanner('cam', '已选择摄像头，正在启动预览...', 'ok');
        // Restart preview with the newly chosen camera.
        try { H.StopPreview(); } catch(e) {}
        setTimeout(previewCtl.restartPreview, 100);
      } else {
        pickerError(configSaveError(status));
      }
    });
  }

  function onCamPickerSkip() {
    if (saving) return;
    document.getElementById('cam-picker-overlay').classList.remove('active');
    showBanner('cam', '将使用当前摄像头设置，可在设置页随时更改。', 'ok');
  }

  return {
    load: loadSettings,
    save: onSaveSettings,
    resetDefaults: onResetDefaults,
    onUnlockTierInput: onUnlockTierInput,
    showCamPicker: showCamPicker,
    onCamPickerConfirm: onCamPickerConfirm,
    onCamPickerSkip: onCamPickerSkip
  };
}
