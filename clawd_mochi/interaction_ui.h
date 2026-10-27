/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  interaction_ui.h — 互动调试界面（独立前端资源）
 *
 *      /ia.css   ← IA_UI_CSS
 *      /ia.js    ← IA_UI_JS
 *
 *  主页面只需：<link rel="stylesheet" href="/ia.css">
 *             <div id="iaSlot"></div><script defer src="/ia.js"></script>
 *
 *  这里有一个真正的「戳一下」按钮：用 pointerdown / pointerup 把按下和松开
 *  分别发给 ESP32，所以网页也能识别长按，和板载 BOOT 按键走同一套状态机。
 *
 *  文件编码必须保持 UTF-8（本文件里的 JS 全部用 \u 转义，纯 ASCII）。
 * ═══════════════════════════════════════════════════════════════════════════
 */
#pragma once

static const char IA_UI_CSS[] PROGMEM = R"rawcss(
#iaSlot{display:flex;flex-direction:column;align-items:center;gap:14px;width:100%}
.iapoke{width:100%;max-width:390px;background:#201408;border:2px solid #c96a3e;
  border-radius:14px;color:#e0a63c;font-family:'Courier New',monospace;font-size:15px;
  font-weight:bold;padding:20px 10px;cursor:pointer;text-align:center;user-select:none;
  transition:all .1s;touch-action:none}
.iapoke.down{transform:scale(.97);background:#3a2410;color:#ffd07a}
.iarows{width:100%;max-width:390px;display:flex;flex-direction:column;gap:5px;
  font-family:'Courier New',monospace;font-size:11px}
.iarow{display:flex;justify-content:space-between;background:#141013;
  border:1px solid #2a2420;border-radius:7px;padding:7px 11px}
.iarow .k{color:#6a6058;letter-spacing:1px}
.iarow .v{color:#e0a63c;font-weight:bold}
.iabtns{display:grid;grid-template-columns:repeat(3,1fr);gap:7px;width:100%;max-width:390px}
.iabtns button{background:#252428;border:1.5px solid #38343a;border-radius:10px;color:#b8b4ac;
  font-family:'Courier New',monospace;font-size:11px;font-weight:bold;padding:11px 4px;
  cursor:pointer;transition:all .12s}
.iabtns button:active:not(:disabled){transform:scale(.94)}
.iabtns button:disabled{opacity:.3;cursor:default}
.iabtns button.wide{grid-column:1 / -1}
.iabtns button.warn{border-color:#7a3a2a;color:#c97a5a}
.iahint{width:100%;max-width:390px;font-size:10px;color:#6a6058;line-height:1.7;padding:0 2px}
.mascot.tappable{cursor:pointer;user-select:none;-webkit-tap-highlight-color:transparent}
.mascot.tappable:active{opacity:.6}
)rawcss";

static const char IA_UI_JS[] PROGMEM = R"rawjs(
(function () {
  'use strict';

  var slot = document.getElementById('iaSlot');
  if (!slot) return;

  slot.innerHTML =
    '<div class="sec">// interaction</div>' +
    '<div class="iapoke" id="iaPoke">\uD83D\uDC46 \u6233\u4E00\u4E0B</div>' +
    '<div class="iarows" id="iaRows"></div>' +
    '<div class="sec">// interaction test</div>' +
    '<div class="iabtns" id="iaBtns"></div>' +
    '<div class="iahint">\u9875\u5934\u90A3\u4E2A\u5C0F\u87F9\u4E5F\u53EF\u4EE5\u6233\u3002'
    + '\u771F\u673A\u4E0A\u677F\u8F7D BOOT \u6309\u952E\uFF08GPIO9\uFF09\u8D70\u7684\u662F\u540C\u4E00\u5957\u72B6\u6001\u673A\u3002</div>';

  var ROWS = [
    ['\u6700\u8FD1\u4E8B\u4EF6', 'last'], ['CLICKS', 'clicks'], ['IDLE', 'idle'],
    ['TOTAL', 'total'], ['EMOTION', 'emotion'], ['PIN', 'pin']
  ];

  function el(id) { return document.getElementById(id); }
  function get(url) { return fetch(url).then(function (r) { return r.json(); }); }

  var rowBox = el('iaRows');
  ROWS.forEach(function (r) {
    var d = document.createElement('div');
    d.className = 'iarow';
    var k = document.createElement('span'); k.className = 'k'; k.textContent = r[0];
    var v = document.createElement('span'); v.className = 'v'; v.id = 'ia_' + r[1]; v.textContent = '--';
    d.appendChild(k); d.appendChild(v);
    rowBox.appendChild(d);
  });

  // ── 「戳一下」：按下 / 松开分开送，网页也能识别长按 ──
  var isDown = false;
  function send(ev) { fetch('/touch?e=' + ev).catch(function () {}); }
  function press() {
    if (isDown) return;
    isDown = true;
    var p = el('iaPoke'); if (p) p.classList.add('down');
    send('down');
  }
  function release() {
    if (!isDown) return;
    isDown = false;
    var p = el('iaPoke'); if (p) p.classList.remove('down');
    send('up');
    setTimeout(refresh, 60);
  }
  function bind(node) {
    if (!node) return;
    node.addEventListener('pointerdown', function (e) { e.preventDefault(); press(); });
    node.addEventListener('pointerup',   function (e) { e.preventDefault(); release(); });
    node.addEventListener('pointercancel', release);
    node.addEventListener('pointerleave', release);
  }
  bind(el('iaPoke'));
  bind(document.querySelector('.hdr .mascot'));
  var msc = document.querySelector('.hdr .mascot');
  if (msc) msc.classList.add('tappable');

  // ── 测试按钮 ──
  function mkBtn(box, label, fn, cls) {
    var b = document.createElement('button');
    b.textContent = label;
    if (cls) b.className = cls;
    b.onclick = fn;
    box.appendChild(b);
  }

  var box = el('iaBtns');
  mkBtn(box, '\u5355\u51FB',       function () { get('/ia/test?e=1').then(apply).catch(function () {}); });
  mkBtn(box, '\u8FDE\u51FB \u00D73', function () { get('/ia/test?e=2').then(apply).catch(function () {}); });
  mkBtn(box, '\u8FC7\u5EA6 \u00D78', function () { get('/ia/test?e=3').then(apply).catch(function () {}); });
  mkBtn(box, '\u957F\u6309',       function () { get('/ia/test?e=4').then(apply).catch(function () {}); });
  mkBtn(box, 'Reset',           function () { get('/ia/reset').then(apply).catch(function () {}); }, 'warn');
  mkBtn(box, '\u8BBE\u5907\u8C03\u8BD5\u5C4F', function () {
    get('/ia/view').then(function () { if (window.toast) toast('\u8BBE\u5907\uFF1A\u4E92\u52A8\u8C03\u8BD5\u5C4F'); }).catch(function () {});
  }, 'wide');

  // ── 读数 ──
  function fmtIdle(ms) {
    if (typeof ms !== 'number') return '--';
    return (ms / 1000).toFixed(1) + 's';
  }

  function apply(j) {
    if (!j) return;
    var m = {
      last:    j.lastLabel || '--',
      clicks:  (j.clicks || 0) + (j.clicks >= 3 ? '  (burst)' : ''),
      idle:    fmtIdle(j.idle),
      total:   j.total || 0,
      emotion: j.emotionLabel || '--',
      pin:     j.pressed ? 'PRESSED' : 'RELEASED'
    };
    for (var k in m) {
      var n = el('ia_' + k);
      if (n) n.textContent = m[k];
    }
  }

  function refresh() {
    if (document.hidden) return;
    get('/ia/list').then(apply).catch(function () {});
  }

  refresh();
  // 整合新增：面板收起时不轮询（展开后 <=1.5s 自动刷新）
  setInterval(function () { if (slot.offsetParent) refresh(); }, 1500);
})();
)rawjs";
