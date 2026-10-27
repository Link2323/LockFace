// The title-bar gear groups color mode and language. This controller owns
// menu visibility and color mode; i18n.js owns the language rows and status.
// The color choice is persisted by the native host
// (HKCU ThemeMode) and applied here as a data-theme attribute on <html> —
// set synchronously during page script evaluation, before the first paint.
// "system" resolves through prefers-color-scheme (WebView2 tracks the Windows
// app mode) and keeps live-tracking OS changes via matchMedia; explicit modes
// pin the page while the host pins the DWM caption and startup fill to the
// same record. deps: {host} — GetThemeMode/SetThemeMode only.
function createThemeCtl(deps) {
  var H = deps.host;
  var MODES = ['light', 'dark', 'system'];
  var mode = 'system';
  var open = false;

  function resolved() {
    if (mode !== 'system') return mode;
    try {
      return (window.matchMedia &&
              window.matchMedia('(prefers-color-scheme: dark)').matches)
        ? 'dark' : 'light';
    } catch (e) { return 'light'; }
  }

  function paint() {
    var root = document.documentElement;
    if (root) {
      root.setAttribute('data-theme', resolved());
    }
    for (var i = 0; i < MODES.length; i++) {
      var row = document.getElementById('theme-row-' + MODES[i]);
      if (row) {
        row.classList.toggle('on', MODES[i] === mode);
        row.setAttribute('aria-checked', String(MODES[i] === mode));
      }
    }
    var btn = document.getElementById('theme-button');
    if (btn) btn.setAttribute('aria-expanded', open ? 'true' : 'false');
  }

  function setOpen(v) {
    if (open === v) return;
    open = v;
    var menu = document.getElementById('theme-menu');
    if (menu) menu.classList.toggle('open', open);
    paint();
  }

  function toggle(ev) {
    if (ev && ev.stopPropagation) ev.stopPropagation();
    setOpen(!open);
  }

  function pick(next) {
    if (MODES.indexOf(next) < 0) return;
    mode = next;
    try { H.SetThemeMode(mode); } catch (e) {}
    setOpen(false);
    paint();
  }

  function init() {
    try {
      var stored = H.GetThemeMode();
      if (MODES.indexOf(stored) >= 0) mode = stored;
    } catch (e) {}
    try {
      if (window.matchMedia) {
        var mq = window.matchMedia('(prefers-color-scheme: dark)');
        var onChange = function() { if (mode === 'system') paint(); };
        if (mq.addEventListener) mq.addEventListener('change', onChange);
        else if (mq.addListener) mq.addListener(onChange);
      }
    } catch (e) {}
    // Outside click and Esc close the menu; the gear click stops propagation
    // in toggle(), clicks inside the menu on the container in index.html.
    document.addEventListener('click', function(ev) {
      if (!open) return;
      var t = ev.target;
      var menu = document.getElementById('theme-menu');
      var btn = document.getElementById('theme-button');
      if (menu && menu.contains(t)) return;
      if (btn && btn.contains(t)) return;
      setOpen(false);
    });
    document.addEventListener('keydown', function(e) {
      if (e.key === 'Escape' && open) setOpen(false);
    });
    paint();
  }

  return {init: init, toggle: toggle, pick: pick,
          mode: function() { return mode; }};
}
