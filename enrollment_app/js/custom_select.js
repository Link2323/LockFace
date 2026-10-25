// Re-skin for native <select> elements. The OS draws select option popups as
// sharp-cornered native menus that page CSS cannot reach, so this module wraps
// each select in a DOM facade:
//   - the real <select> stays in the tree (hidden) as the single source of
//     truth — existing code keeps using .value, inline onchange and option
//     mutation exactly as before;
//   - the facade trigger mirrors the current label; the popup renders the
//     options with the app's corner/typography system;
//   - option-list mutations sync through a MutationObserver; direct .value /
//     .selectedIndex writes fire no events, so call sites that re-point a
//     select programmatically call refreshAll() afterwards (settings does).
// deps: none. Activation happens in app_main (enhanceAll); in the headless
// page test the document mock has no <select> elements, so enhancing no-ops.
function createCustomSelectModule() {
  var states = [];
  var openState = null;

  function labelText(state) {
    var opt = state.sel.options[state.sel.selectedIndex];
    return opt ? opt.textContent : '';
  }

  function renderTrigger(state) {
    state.label.textContent = labelText(state);
  }

  function paintActive(state) {
    var rows = state.pop.children;
    for (var i = 0; i < rows.length; i++) {
      var cls = 'cselect-row';
      if (i === state.sel.selectedIndex) cls += ' cselect-row-on';
      if (i === state.active) cls += ' cselect-row-active';
      rows[i].className = cls;
    }
  }

  function renderRows(state) {
    state.pop.innerHTML = '';
    var opts = state.sel.options;
    for (var i = 0; i < opts.length; i++) {
      var row = document.createElement('div');
      row.textContent = opts[i].textContent;
      (function(idx) {
        row.addEventListener('click', function(ev) {
          ev.stopPropagation();
          choose(state, idx);
        });
      })(i);
      state.pop.appendChild(row);
    }
    state.active = state.sel.selectedIndex;
    paintActive(state);
  }

  function choose(state, idx) {
    if (idx >= 0 && idx < state.sel.options.length &&
        idx !== state.sel.selectedIndex) {
      state.sel.selectedIndex = idx;
      // selectedIndex writes fire no events the observer could see — update
      // the trigger here. The change event is for inline handlers/modules.
      renderTrigger(state);
      try { state.sel.dispatchEvent(new Event('change')); } catch (e) {}
    }
    close();
  }

  // Fixed positioning (below the trigger, flipping above when the viewport
  // edge is near) so inner scrollers like .settings-scroll cannot clip it.
  function place(state) {
    var r = state.trigger.getBoundingClientRect();
    var vh = window.innerHeight || 700;
    state.pop.style.minWidth = r.width + 'px';
    state.pop.style.left = r.left + 'px';
    state.pop.style.visibility = 'hidden';
    state.pop.style.display = 'block';
    var h = state.pop.offsetHeight;
    if (r.bottom + h + 6 > vh && r.top - h - 6 > 0) {
      state.pop.style.top = (r.top - h - 4) + 'px';
    } else {
      state.pop.style.top = (r.bottom + 4) + 'px';
    }
    state.pop.style.visibility = '';
  }

  function open(state) {
    if (openState === state) return;
    close();
    renderRows(state);
    place(state);
    state.wrap.classList.add('cselect-open');
    openState = state;
  }

  function close() {
    if (!openState) return;
    openState.pop.style.display = 'none';
    openState.wrap.classList.remove('cselect-open');
    openState = null;
  }

  function toggle(state) {
    if (openState === state) close(); else open(state);
  }

  // One global listener set for all facades, installed with the first.
  function installGlobal() {
    document.addEventListener('click', function(ev) {
      if (openState && !openState.wrap.contains(ev.target)) close();
    });
    window.addEventListener('blur', close);
    window.addEventListener('resize', close);
    // Any scroll (capture: includes inner scrollers) detaches a fixed popup
    // from its trigger — except scrolling the open list itself.
    window.addEventListener('scroll', function(ev) {
      if (openState && openState.pop.contains(ev.target)) return;
      close();
    }, true);
  }

  function enhance(sel) {
    if (!sel || sel._cselectDone) return;
    sel._cselectDone = true;
    var wrap = document.createElement('div');
    wrap.className = 'cselect';
    sel.parentNode.insertBefore(wrap, sel);
    wrap.appendChild(sel);
    sel.className += ' cselect-source';
    var trigger = document.createElement('div');
    trigger.className = 'cselect-trigger';
    trigger.tabIndex = 0;
    var label = document.createElement('span');
    label.className = 'cselect-label';
    trigger.appendChild(label);
    var pop = document.createElement('div');
    pop.className = 'cselect-pop';
    pop.style.display = 'none';
    wrap.appendChild(trigger);
    wrap.appendChild(pop);
    var state = {sel: sel, trigger: trigger, pop: pop, wrap: wrap,
                 label: label, active: 0};

    trigger.addEventListener('click', function(ev) {
      ev.stopPropagation();
      toggle(state);
    });
    trigger.addEventListener('keydown', function(ev) {
      if (openState !== state) {
        if (ev.key === 'Enter' || ev.key === ' ' ||
            ev.key === 'ArrowDown' || ev.key === 'ArrowUp') {
          ev.preventDefault();
          open(state);
        }
        return;
      }
      if (ev.key === 'ArrowDown') {
        ev.preventDefault();
        state.active = Math.min(state.active + 1, state.pop.children.length - 1);
        paintActive(state);
      } else if (ev.key === 'ArrowUp') {
        ev.preventDefault();
        state.active = Math.max(state.active - 1, 0);
        paintActive(state);
      } else if (ev.key === 'Enter' || ev.key === ' ') {
        ev.preventDefault();
        choose(state, state.active);
      } else if (ev.key === 'Escape') {
        close();
      }
    });
    // Option lists are rebuilt by settings.js (innerHTML/appendChild); the
    // observer keeps the facade in step without touching those call sites.
    if (typeof MutationObserver === 'function') {
      new MutationObserver(function() { refresh(state); })
        .observe(sel, {childList: true});
    }
    renderTrigger(state);
    states.push(state);
    if (states.length === 1) installGlobal();
  }

  function refresh(state) {
    renderTrigger(state);
    if (openState === state) renderRows(state);
  }

  function enhanceAll() {
    var list = document.querySelectorAll('select');
    for (var i = 0; i < list.length; i++) enhance(list[i]);
  }

  function refreshAll() {
    for (var i = 0; i < states.length; i++) refresh(states[i]);
  }

  return {enhanceAll: enhanceAll, refreshAll: refreshAll};
}
