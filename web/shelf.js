/* SPDX-License-Identifier: GPL-3.0-or-later */
function bindShelfNavigation(grid, tv, select, back) {
  var lastKey = 0;
  var directions = {37: 'Left', 38: 'Up', 39: 'Right', 40: 'Down'};
  function activeScope() {
    if (!tv) return grid;
    return document.querySelector('dialog[open]') || document.querySelector('.toolbar-more[open]') ||
      (document.querySelector('#legacy-tools:not([hidden])') ? document.querySelector('header') : document);
  }
  function focus(el) {
    if (!el) return;
    el.focus();
    var id = el.getAttribute('data-g');
    if (id) { select(id); keepVisible(el); }
  }
  /* Choosing a row grows it and closes the one before, which moves both: bring it back into view. */
  function keepVisible(el) {
    var header = document.querySelector('header');
    var top = header && window.getComputedStyle(header).position === 'sticky' ? header.getBoundingClientRect().bottom : 0;
    var box = el.getBoundingClientRect();
    var under = box.bottom - window.innerHeight, over = box.top - top;
    if (over < 0) window.scrollBy(0, over);
    else if (under > 0) window.scrollBy(0, Math.min(under, over));
  }
  /* Tab chooses the row it lands on too; a pointer chooses on click, so rows never move under it. */
  grid.addEventListener('keyup', function (e) {
    var id = e.keyCode === 9 && e.target.getAttribute('data-g');
    if (id) { select(id); keepVisible(e.target); }
  });
  function gridMove(from, direction) {
    var card = from.parentNode;
    if (!card || card.parentNode !== grid) return null;
    var origin = from.getBoundingClientRect();
    var ox = origin.left + origin.width / 2;
    var oy = origin.top + origin.height / 2;
    var forward = direction === 'Right' || direction === 'Down';
    var horiz = direction === 'Left' || direction === 'Right';
    var step = forward ? 'nextElementSibling' : 'previousElementSibling';
    var cur = card[step];
    var best = null, bestScore = Infinity, sawOtherRow = false;
    while (cur) {
      var btn = cur.querySelector('[data-g]:not([disabled])');
      if (btn && btn.getClientRects().length) {
        var r = btn.getBoundingClientRect();
        var dx = r.left + r.width / 2 - ox;
        var dy = r.top + r.height / 2 - oy;
        if (horiz) {
          if (Math.abs(dy) <= Math.max(origin.height, r.height) * 0.5 && (forward ? dx : -dx) >= 3)
            return btn;
          return null;
        }
        var along = (forward ? dy : -dy);
        if (along >= 3) {
          var across = Math.abs(dx);
          var score = along + across * 3 + (across > along ? 160 : 0);
          if (score < bestScore) { best = btn; bestScore = score; }
          if (sawOtherRow && along > Math.max(origin.height, r.height) * 1.5) break;
          sawOtherRow = true;
        }
      }
      cur = cur[step];
    }
    return best;
  }
  function move(direction) {
    var axis = direction === 'Left' || direction === 'Right' ? 'x' : 'y';
    var sign = direction === 'Left' || direction === 'Up' ? -1 : 1;
    var scope = activeScope();
    var from = document.activeElement;
    if (from && from.getAttribute && from.getAttribute('data-g') && scope.contains(grid) && grid.contains(from)) {
      var fast = gridMove(from, direction);
      if (fast) { focus(fast); return; }
    }
    var controls = Array.prototype.filter.call(
      scope.querySelectorAll('button:not([disabled]),input:not([disabled]),select:not([disabled]),textarea:not([disabled]),a[href],[tabindex="0"]'),
      function (el) { return el.getClientRects().length; });
    if (controls.indexOf(from) < 0) {
      var cover = scope.querySelector('[data-g][aria-pressed="true"]');
      focus(controls.indexOf(cover) >= 0 ? cover : controls[0]);
      return;
    }
    var origin = from.getBoundingClientRect();
    var best, bestScore = Infinity;
    controls.forEach(function (el) {
      if (el === from) return;
      var rect = el.getBoundingClientRect();
      var dx = rect.left + rect.width / 2 - origin.left - origin.width / 2;
      var dy = rect.top + rect.height / 2 - origin.top - origin.height / 2;
      var along = (axis === 'x' ? dx : dy) * sign;
      if (along < 3) return;
      var across = Math.abs(axis === 'x' ? dy : dx);
      var score = along + across * 3 + (across > along ? 160 : 0);
      if (score < bestScore) { best = el; bestScore = score; }
    });
    focus(best);
  }
  document.addEventListener('keydown', function (e) {
    var direction = directions[e.keyCode] || (/^Arrow(Left|Up|Right|Down)$/.test(e.key) ? e.key.slice(5) : '');
    /* Up/Down leave a one-line text field (the search): it has no use for them, and on the
     * PS4 a field that keeps them traps the controller */
    var line = e.target.tagName === 'INPUT' && /^(search|text)$/i.test(e.target.type) && (direction === 'Up' || direction === 'Down');
    var editable = (/^(INPUT|SELECT|TEXTAREA)$/.test(e.target.tagName) && !line) || e.target.isContentEditable;
    if (e.defaultPrevented || e.metaKey || e.ctrlKey || e.altKey) return;
    if (tv && (e.key === 'Escape' || e.keyCode === 27)) {
      lastKey = Date.now();
      e.preventDefault();
      back();
    } else if (direction && !editable && (tv || grid.contains(e.target))) {
      lastKey = Date.now();
      e.preventDefault();
      move(direction);
    } else if (tv && (e.key === 'Enter' || e.keyCode === 13)) {
      lastKey = Date.now();
    }
  });

  if (!tv || !navigator.getGamepads || !window.requestAnimationFrame) return;
  var previous = '', next = 0, repeatSteps = 0, confirmed = false, cancelled = false;
  function poll(now) {
    if (document.hidden) { window.requestAnimationFrame(poll); return; }
    var pads, pad;
    try { pads = navigator.getGamepads(); } catch (err) { return; }
    if (!pads) return;
    window.requestAnimationFrame(poll);
    for (var i = 0; i < pads.length; i++) if (pads[i]) { pad = pads[i]; break; }
    if (!pad) { previous = ''; repeatSteps = 0; confirmed = cancelled = false; return; }
    function pressed(index) {
      var button = pad.buttons[index];
      return typeof button === 'number' ? button > 0.5 : !!(button && button.pressed);
    }
    var ay = pad.axes[1] || 0, ax = pad.axes[0] || 0;
    var direction = pressed(12) || (previous === 'Up' ? ay < -.45 : ay < -.65) ? 'Up' :
      pressed(13) || (previous === 'Down' ? ay > .45 : ay > .65) ? 'Down' :
      pressed(14) || (previous === 'Left' ? ax < -.45 : ax < -.65) ? 'Left' :
      pressed(15) || (previous === 'Right' ? ax > .45 : ax > .65) ? 'Right' : '';
    var ok = pressed(0), cancel = pressed(1);
    // Some dialog engines also surface the same physical input as native keys.
    if (Date.now() - lastKey > 250) {
      if (direction && (direction !== previous || now >= next)) {
        move(direction);
        if (direction !== previous) {
          repeatSteps = 0;
          next = now + 250;
        } else {
          repeatSteps++;
          var inGrid = document.activeElement && grid.contains(document.activeElement);
          var interval = inGrid ? (repeatSteps < 6 ? 85 : 50) : 110;
          next = now + interval;
        }
      } else if (!direction) {
        repeatSteps = 0;
      }
      if (ok && !confirmed) {
        var el = document.activeElement;
        if (el.tagName === 'SELECT' && el.options.length) {
          el.selectedIndex = (el.selectedIndex + 1) % el.options.length;
          var change = document.createEvent('HTMLEvents');
          change.initEvent('change', true, false);
          el.dispatchEvent(change);
        } else if (el !== document.body && !el.disabled) el.click();
        else move('Down');
      }
      if (cancel && !cancelled) back();
    }
    previous = direction;
    confirmed = ok;
    cancelled = cancel;
  }
  window.requestAnimationFrame(poll);
}
