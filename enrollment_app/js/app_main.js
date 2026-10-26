// Caption actions post native messages so closing cannot destroy the WebView
// while its synchronous host-object invocation is still on the stack.
function minimizeWindow() {
  window.chrome.webview.hostObjects.sync.host.MinimizeWindow();
}
function closeWindow() {
  window.chrome.webview.hostObjects.sync.host.CloseWindow();
}

// ================================================================
// Composition root + bootstrap — the only file with executable top-level
// code. Every other module is a factory whose deps parameter declares its
// exact imports; the build order below IS the dependency order. The two
// cycles in the module graph are broken with registration hooks instead of
// direct constructor references:
//   nav.onEnter            — settings/faces/logs register their tab-entry
//                            loaders (each needs switchTab from navigation)
//   accountRefresh.onChanged — faces registers its post-refresh reload
//                            (faces needs accountRefresh.check on load)
// ================================================================
function buildEnrollmentApp(deps) {
  var H = deps.host;
  var timers = deps.timers || window;

  var ui = createUiCommon({host: H});
  var previewCtl = createPreview({host: H, showBanner: ui.showBanner});
  var consent = createConsentGate({host: H, previewCtl: previewCtl, ui: ui});
  var captureCtl = createCaptureCtl({timers: timers});
  var nav = createNavigation({host: H, captureCtl: captureCtl,
                              previewCtl: previewCtl, showBanner: ui.showBanner});
  var accountRefresh = createAccountRefreshModule({host: H, showBanner: ui.showBanner,
                                                   withBusy: ui.withBusy});
  var faces = createFacesModule({host: H, showBanner: ui.showBanner,
                                 showAppModal: ui.showAppModal,
                                 escapeHtml: ui.escapeHtml,
                                 switchTab: nav.switchTab,
                                 checkAccountRefresh: accountRefresh.check});
  var enrollSave = createEnrollSaveModule({host: H, showBanner: ui.showBanner,
                                           setCaption: ui.setCaption,
                                           withBusy: ui.withBusy,
                                           captureCtl: captureCtl,
                                           showScreen: nav.showScreen,
                                           previewCtl: previewCtl});
  var captureUi = createCaptureUi({host: H, showBanner: ui.showBanner,
                                   setCaption: ui.setCaption,
                                   captureCtl: captureCtl,
                                   switchTab: nav.switchTab,
                                   showScreen: nav.showScreen,
                                   previewCtl: previewCtl,
                                   requireConsent: consent.ensure,
                                   showAppendScreen: enrollSave.showAppendScreen,
                                   showRefreshPrompt: accountRefresh.showRefreshPrompt});
  var settings = createSettingsModule({host: H, showBanner: ui.showBanner,
                                       previewCtl: previewCtl,
                                       syncSelects: deps.syncSelects});
  var logs = createLogsModule({host: H, escapeHtml: ui.escapeHtml});
  var theme = createThemeCtl({host: H});

  nav.onEnter('settings', settings.load);
  nav.onEnter('faces', faces.load);
  nav.onEnter('logs', logs.refresh);
  accountRefresh.onChanged(faces.load);

  return {
    host: H,
    ui: ui,
    previewCtl: previewCtl,
    consent: consent,
    captureCtl: captureCtl,
    nav: nav,
    accountRefresh: accountRefresh,
    faces: faces,
    enrollSave: enrollSave,
    captureUi: captureUi,
    settings: settings,
    logs: logs,
    theme: theme
  };
}

// Bootstrap: build against the real WebView2 host, publish the event surface
// consumed by index.html.in's inline handlers, and start the page. The var
// list below is the page's ENTIRE global surface (plus consoleApp itself) —
// everything else now lives inside module closures.
var customSelect = createCustomSelectModule();
var consoleApp = buildEnrollmentApp({
  host: window.chrome.webview.hostObjects.sync.host,
  timers: window,
  syncSelects: customSelect.refreshAll
});

var switchTab = consoleApp.nav.switchTab,
    onCapture = consoleApp.captureUi.onCapture,
    onSave = consoleApp.enrollSave.onSave,
    onPwdCancel = consoleApp.enrollSave.onPwdCancel,
    onAppendConfirm = consoleApp.enrollSave.onAppendConfirm,
    onAppendCancel = consoleApp.enrollSave.onAppendCancel,
    goAddFace = consoleApp.faces.goAddFace,
    onRenameFace = consoleApp.faces.onRenameFace,
    onDeleteFace = consoleApp.faces.onDeleteFace,
    onClearAllFaces = consoleApp.faces.onClearAllFaces,
    onSaveSettings = consoleApp.settings.save,
    onResetDefaults = consoleApp.settings.resetDefaults,
    onUnlockTierInput = consoleApp.settings.onUnlockTierInput,
    onCamPickerConfirm = consoleApp.settings.onCamPickerConfirm,
    onCamPickerSkip = consoleApp.settings.onCamPickerSkip,
    refreshLogs = consoleApp.logs.refresh,
    copyLogs = consoleApp.logs.copyLogs,
    clearLogs = consoleApp.logs.clearLogs,
    onRefreshConfirm = consoleApp.accountRefresh.onRefreshConfirm,
    onRefreshDismiss = consoleApp.accountRefresh.onRefreshDismiss,
    openLicenseViewer = consoleApp.ui.openLicenseViewer,
    closeLicenseViewer = consoleApp.ui.closeLicenseViewer,
    showLicenseDocument = consoleApp.ui.showLicenseDocument,
    onConsentAccept = consoleApp.consent.accept,
    onConsentDecline = consoleApp.consent.decline,
    onConsentStatus = consoleApp.consent.statusMenu,
    openPrivacyNotice = consoleApp.consent.openPrivacyNotice,
    onModalOk = consoleApp.ui.onModalOk,
    onModalCancel = consoleApp.ui.onModalCancel,
    onModalMid = consoleApp.ui.onModalMid,
    toggleThemeMenu = consoleApp.theme.toggle,
    onThemePick = consoleApp.theme.pick;

consoleApp.ui.bindEnterKey('fld-pass', 'btn-save', consoleApp.enrollSave.onSave);
consoleApp.ui.bindEnterKey('fld-face-label', 'btn-save', consoleApp.enrollSave.onSave);
consoleApp.ui.bindEnterKey('append-face-label', 'btn-append', consoleApp.enrollSave.onAppendConfirm);
consoleApp.ui.bindEnterKey('refresh-pass', 'refresh-confirm', consoleApp.accountRefresh.onRefreshConfirm);

(function init() {
  var H = consoleApp.host;
  var nav = consoleApp.nav, ui = consoleApp.ui;
  var previewCtl = consoleApp.previewCtl, captureCtl = consoleApp.captureCtl;
  var logs = consoleApp.logs, settings = consoleApp.settings;
  var theme = consoleApp.theme;
  var accountRefresh = consoleApp.accountRefresh;
  var consent = consoleApp.consent;

  // First synchronous action: the data-theme attribute must land before the
  // first paint so an explicit dark preference never flashes light.
  theme.init();

  customSelect.enhanceAll();

  try { document.getElementById('fld-user').value = H.GetUsername(); } catch(e) {}

  // The camera must not start before the per-user consent gate is accepted.
  // The startup prompts are deferred with it so they cannot cover the consent
  // dialog or race its overlay.
  consent.init(function() {
    // StartPreview is a synchronous host call — opening the device plus
    // loading the four ONNX sessions can hold the renderer for seconds.
    // Defer it past page load + reveal so the UI comes up first; the
    // cam-loading overlay shown by markCameraPending stands in ("正在启动
    // 摄像头...") until the first frame arrives. The tab guard keeps a user
    // who switched away within the delay from powering the camera on
    // behind another screen. The prompt timers below keep their head start
    // so they still land before the camera takes over.
    setTimeout(function() {
      if (nav.tab() === 'cam') previewCtl.restartPreview();
    }, 450);

    // If the account was converted from MSA→local with a stale record, surface
    // the refresh prompt at startup (before the camera preview takes over).
    setTimeout(function() { accountRefresh.check(); }, 150);

    // First-run camera picker: if config has no camera_device AND multiple
    // cameras exist, prompt the user to choose one. Fresh installs (or users
    // who never touched settings) may not know the camera dropdown exists.
    setTimeout(function() {
      try {
        var cfg = JSON.parse(H.GetConfig());
        if (!cfg.camera_device) settings.showCamPicker();
      } catch(e) {}
    }, 300);
  });

  setInterval(function() {
    if (!previewCtl.isViewing()) return;
    if (captureCtl.isActive()) return;  // sequence in flight — the capture poll owns the tag
    try {
      var st = JSON.parse(H.GetCaptureStatus());
      // Only after the whole sequence has ended do we show the running total.
      // A cancelled capture (st.cancelled) leaves a partial total behind —
      // don't resurrect it after the user asked to reset to zero. Same for
      // an ended capture flow: the total is stale once the
      // confirm/cancel/save screen was left.
      if (st.done && !st.livenessChecking && !st.cancelled && captureCtl.canShowIdleTotal()) {
        ui.setCaption('已采 ' + st.total + ' 帧');
      }
    } catch(e) {}
  }, 300);

  // Auto-refresh log viewer every 2 seconds when logs tab is active
  setInterval(function() {
    if (nav.tab() === 'logs') logs.refresh();
  }, 2000);
})();
