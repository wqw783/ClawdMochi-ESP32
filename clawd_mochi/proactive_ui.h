/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  proactive_ui.h — 主动互动调试界面（独立前端资源）
 *
 *      /pro.css   ← PRO_UI_CSS
 *      /pro.js    ← PRO_UI_JS
 *
 *  主页面只需：<link rel="stylesheet" href="/pro.css">
 *             <div id="proSlot"></div><script defer src="/pro.js"></script>
 *
 *  文件编码必须保持 UTF-8（本文件里的 JS 全部用 \u 转义，纯 ASCII）。
 * ═══════════════════════════════════════════════════════════════════════════
 */
#pragma once

static const char PRO_UI_CSS[] PROGMEM = R"rawcss(
#proSlot{display:flex;flex-direction:column;align-items:center;gap:14px;width:100%}
.prost{width:100%;max-width:390px;background:#141013;border:1.5px solid #2a2420;
  border-radius:10px;padding:11px 14px;display:flex;justify-content:space-between;
  align-items:center;font-family:'Courier New',monospace}
.prost .lbl{font-size:10px;color:#6a6058;letter-spacing:2px}
.prost .val{font-size:15px;font-weight:bold;letter-spacing:1px}
.prost .on{color:#28b878}
.prost .off{color:#6a6058}
.prorows{width:100%;max-width:390px;display:flex;flex-direction:column;gap:5px;
  font-family:'Courier New',monospace;font-size:11px}
.prorow{display:flex;justify-content:space-between;background:#141013;
  border:1px solid #2a2420;border-radius:7px;padding:7px 11px}
.prorow .k{color:#6a6058;letter-spacing:1px}
.prorow .v{color:#e0a63c;font-weight:bold}
.probtns{display:grid;grid-template-columns:repeat(3,1fr);gap:7px;width:100%;max-width:390px}
.probtns button{background:#252428;border:1.5px solid #38343a;border-radius:10px;color:#b8b4ac;
  font-family:'Courier New',monospace;font-size:11px;font-weight:bold;padding:11px 4px;
  cursor:pointer;transition:all .12s}
.probtns button:active:not(:disabled){transform:scale(.94)}
.probtns button:disabled{opacity:.3;cursor:default}
.probtns button.wide{grid-column:1 / -1}
.probtns button.on{border-color:#28b878;color:#28b878;background:#0c1e12}
.probtns button.warn{border-color:#7a3a2a;color:#c97a5a}
.prohint{width:100%;max-width:390px;font-size:10px;color:#6a6058;line-height:1.7;padding:0 2px}
)rawcss";

static const char PRO_UI_JS[] PROGMEM = R"rawjs(
(function () {
  'use strict';

  var slot = document.getElementById('proSlot');
  if (!slot) return;

  slot.innerHTML =
    '<div class="sec">// proactive</div>' +
    '<div class="prost"><span class="lbl">PROACTIVE INTERACTION</span>' +
    '<span class="val" id="proSt">--</span></div>' +
    '<div class="prorows" id="proRows"></div>' +
    '<div class="sec">// proactive test</div>' +
    '<div class="probtns" id="proBtns"></div>' +
    '<div class="prohint">\u4E3B\u52A8\u884C\u4E3A\u53EA\u5728\u8BBE\u5907\u505C\u5728'
    + '\u300C\u8868\u60C5\u89C6\u56FE\u300D\u65F6\u624D\u4F1A\u6F14\uFF1B\u4E0D\u5728\u90A3\u4E2A'
    + '\u89C6\u56FE\u4E0B\u4F1A\u8DF3\u8FC7\u5E76\u91CD\u65B0\u6390\u65F6\u95F4\u3002</div>';

  var ROWS = [
    ['IDLE', 'idle'], ['EMOTION', 'emotion'], ['\u5F53\u524D\u884C\u4E3A', 'action'],
    ['\u4E0B\u6B21\u884C\u4E3A', 'next'], ['TRIGGER IN', 'trigger'], ['\u8DDD\u4E0A\u6B21', 'last']
  ];

  function el(id) { return document.getElementById(id); }
  function get(url) { return fetch(url).then(function (r) { return r.json(); }); }

  var rowBox = el('proRows');
  ROWS.forEach(function (r) {
    var d = document.createElement('div');
    d.className = 'prorow';
    var k = document.createElement('span'); k.className = 'k'; k.textContent = r[0];
    var v = document.createElement('span'); v.className = 'v'; v.id = 'pro_' + r[1]; v.textContent = '--';
    d.appendChild(k); d.appendChild(v);
    rowBox.appendChild(d);
  });

  function fmtMs(ms) {
    if (typeof ms !== 'number') return '--';
    return (ms / 1000).toFixed(1) + 's';
  }

  function apply(j) {
    if (!j) return;
    var st = el('proSt');
    if (st) {
      st.textContent = j.enabled ? 'ON' : 'OFF';
      st.className = 'val ' + (j.enabled ? 'on' : 'off');
    }
    var m = {
      idle:    fmtMs(j.idle),
      emotion: j.emotionLabel || '--',
      action:  j.actionLabel || '--',
      next:    j.nextLabel || '--',
      trigger: j.enabled ? fmtMs(j.triggerIn) : 'OFF',
      last:    j.everFired ? fmtMs(j.sinceLast) : '\u2014'
    };
    for (var k in m) {
      var n = el('pro_' + k);
      if (n) n.textContent = m[k];
    }
  }

  function refresh() {
    if (document.hidden) return;
    get('/pro/list').then(apply).catch(function () {});
  }

  function mkBtn(box, label, fn, cls) {
    var b = document.createElement('button');
    b.textContent = label;
    if (cls) b.className = cls;
    b.onclick = fn;
    box.appendChild(b);
  }

  var box = el('proBtns');
  mkBtn(box, '\u7ACB\u5373\u89E6\u53D1', function () {
    get('/pro/trigger').then(function (j) {
      if (j && j.started) { if (window.toast) toast('proactive: fired'); }
      else if (window.toast) {
        toast('\u672A\u89E6\u53D1\uFF1A\u9700\u505C\u5728\u8868\u60C5\u89C6\u56FE\u4E14\u672A\u9501\u5B9A', false);
      }
      refresh();
    }).catch(function () {});
  });
  mkBtn(box, '\u5F00\u542F', function () { get('/pro/set?on=1').then(apply).catch(function () {}); }, 'on');
  mkBtn(box, '\u5173\u95ED', function () { get('/pro/set?on=0').then(apply).catch(function () {}); }, 'warn');
  mkBtn(box, '\u91CD\u6390\u8BA1\u65F6', function () { get('/pro/reset').then(apply).catch(function () {}); });
  mkBtn(box, '\u8BBE\u5907\u8C03\u8BD5\u5C4F', function () {
    get('/pro/view').then(function () {
      if (window.toast) toast('\u8BBE\u5907\uFF1A\u4E3B\u52A8\u4E92\u52A8\u8C03\u8BD5\u5C4F');
    }).catch(function () {});
  }, 'wide');

  refresh();
  // 整合新增：面板收起时不轮询（展开后 <=1.5s 自动刷新）
  setInterval(function () { if (slot.offsetParent) refresh(); }, 1500);
})();
)rawjs";
