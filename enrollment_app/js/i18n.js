// The build embeds locales/*.json before this module. Tests inject the same
// catalogs. Keys are stable; dynamic values use named {parameters}.
var lfLocale = 'zh-CN';
try {
  lfLocale = window.chrome.webview.hostObjects.sync.host.GetUiLanguage();
} catch (e) {}

function lfT(key, parameters) {
  var catalog = LF_MESSAGES[lfLocale] || LF_MESSAGES['zh-CN'];
  var text = catalog[key] || LF_MESSAGES['zh-CN'][key] || key;
  return text.replace(/\{(\w+)\}/g, function(match, name) {
    return parameters && Object.prototype.hasOwnProperty.call(parameters, name)
      ? String(parameters[name]) : match;
  });
}

function lfStoredLabel(label) {
  // These angle names are stored in existing databases. Never change their
  // values or translate user-supplied names when changing the UI language.
  var keys = {'正面': 'front', '左转': 'left', '右转': 'right'};
  return keys[label] ? lfT(keys[label]) : label;
}

function lfApplyLanguage() {
  document.documentElement.setAttribute('lang', lfLocale);
  document.querySelectorAll('[data-i18n]').forEach(function(el) {
    el.textContent = lfT(el.getAttribute('data-i18n'));
  });
  ['title', 'placeholder', 'aria-label'].forEach(function(attr) {
    document.querySelectorAll('[data-i18n-' + attr + ']').forEach(function(el) {
      el.setAttribute(attr, lfT(el.getAttribute('data-i18n-' + attr)));
    });
  });
  var mode = 'system';
  try { mode = window.chrome.webview.hostObjects.sync.host.GetLanguageMode(); } catch (e) {}
  lfPaintLanguageMode(mode);
}

function lfPaintLanguageMode(mode) {
  var modes = ['system', 'zh-CN', 'en'];
  if (modes.indexOf(mode) < 0) mode = 'system';
  modes.forEach(function(value) {
    var row = document.getElementById('language-row-' + value);
    if (!row) return;
    row.classList.toggle('on', value === mode);
    row.setAttribute('aria-checked', String(value === mode));
  });
}

function onLanguagePick(mode) {
  var host = window.chrome.webview.hostObjects.sync.host;
  var ok = false;
  try { ok = host.SetLanguageMode(mode) === true; } catch (e) {}
  if (ok) lfPaintLanguageMode(mode);
  var status = document.getElementById('language-status');
  if (status) {
    status.textContent = lfT(ok ? 'language_saved_restart' : 'language_save_failed');
    status.classList.toggle('error', !ok);
    status.hidden = false;
  }
}
