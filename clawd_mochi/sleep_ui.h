/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  sleep_ui.h — 睡眠调试界面（独立前端资源）
 *
 *      /sleep.css   ← SL_UI_CSS
 *      /sleep.js    ← SL_UI_JS
 *
 *  主页面只需：<link rel="stylesheet" href="/sleep.css">
 *             <div id="sleepSlot"></div><script defer src="/sleep.js"></script>
 *
 *  这里全部是**开发测试按钮**，真机上不可能等到 10 分钟才验一次：
 *      现在睡 / 现在醒 / 强制犯困 / 开·关睡眠 / 重置计时 / 测试档
 *      + 模拟时间 22:59 / 23:00 / 02:00 / 06:59 / 07:00
 *
 *  文件编码必须保持 UTF-8（本文件里的 JS 全部用 \u 转义，纯 ASCII）。
 * ═══════════════════════════════════════════════════════════════════════════
 */
#pragma once

static const char SL_UI_CSS[] PROGMEM = R"rawcss(
#sleepSlot{display:flex;flex-direction:column;align-items:center;gap:14px;width:100%}
.slrows{width:100%;max-width:390px;display:flex;flex-direction:column;gap:5px;
  font-family:'Courier New',monospace;font-size:11px}
.slrow{display:flex;justify-content:space-between;background:#141013;
  border:1px solid #2a2420;border-radius:7px;padding:7px 11px}
.slrow .k{color:#6a6058;letter-spacing:1px}
.slrow .v{color:#7fb8e0;font-weight:bold}
.slrow .v.sleep{color:#28b878}
.slbtns{display:grid;grid-template-columns:repeat(3,1fr);gap:7px;width:100%;max-width:390px}
.slbtns button{background:#252428;border:1.5px solid #38343a;border-radius:10px;color:#b8b4ac;
  font-family:'Courier New',monospace;font-size:11px;font-weight:bold;padding:11px 4px;
  cursor:pointer;transition:all .12s}
.slbtns button:active{transform:scale(.94)}
.slbtns button.on{border-color:#28b878;color:#28b878;background:#0c1e12}
.slbtns button.off{border-color:#7a3a2a;color:#c97a5a}
.slbtns button.warn{border-color:#7a3a2a;color:#c97a5a}
.slbtns button.wide{grid-column:1 / -1}
.sltimes{display:flex;flex-wrap:wrap;gap:6px;width:100%;max-width:390px}
.sltimes button{flex:1 1 auto;background:#141013;border:1.5px solid #2a2420;border-radius:8px;
  color:#8a8278;font-family:'Courier New',monospace;font-size:11px;font-weight:bold;
  padding:8px 6px;cursor:pointer;transition:all .12s}
.sltimes button:active{transform:scale(.94)}
.sltimes button.on{border-color:#7fb8e0;color:#7fb8e0;background:#0c1a26}
.slhint{width:100%;max-width:390px;font-size:10px;color:#6a6058;line-height:1.7;padding:0 2px}
)rawcss";

static const char SL_UI_JS[] PROGMEM = R"rawjs(
(function () {
  'use strict';

  var slot = document.getElementById('sleepSlot');
  if (!slot) return;

  slot.innerHTML =
    '<div class="sec">// sleep</div>' +
    '<div class="slrows" id="slRows"></div>' +
    '<div class="sec">// sleep test</div>' +
    '<div class="slbtns" id="slBtns"></div>' +
    '<div class="sec">// test time</div>' +
    '<div class="sltimes" id="slTimes"></div>' +
    '<div class="slhint">'
    + '\u7761\u7740\u7684\u684C\u5BA0\u4F1A\u6682\u505C\u4E3B\u52A8\u4E92\u52A8\u3002'
    + '\u70B9\u4E00\u4E0B\u5B83\u5C31\u4F1A\u9192\uFF0C\u9192\u540E 45 \u79D2\u5185'
    + '\u4E0D\u4F1A\u518D\u7761\u3002</div>'
    + '<div class="slhint">'
    + '\u9608\u503C\uFF1A\u767D\u5929 idle 5 \u5206\u949F / 10 \u5206\u949F\uFF1B'
    + '\u591C\u95F4 45 \u79D2 / 2 \u5206\u949F\u3002'
    + '\u6D4B\u8BD5\u6863\u662F 60 \u79D2 / 80 \u79D2\u3002</div>';

  var ROWS = [
    ['STATE', 'state'], ['REASON', 'reason'], ['CLOCK', 'clock'],
    ['ENABLED', 'enabled'], ['IDLE', 'idle'], ['DROWSY IN', 'drowsyIn'],
    ['SLEEP IN', 'sleepIn'], ['COOLDOWN', 'cooldown']
  ];

  function el(id) { return document.getElementById(id); }
  function get(url) { return fetch(url).then(function (r) { return r.json(); }); }

  var rowBox = el('slRows');
  ROWS.forEach(function (r) {
    var d = document.createElement('div');
    d.className = 'slrow';
    var k = document.createElement('span'); k.className = 'k'; k.textContent = r[0];
    var v = document.createElement('span'); v.className = 'v'; v.id = 'sl_' + r[1]; v.textContent = '--';
    d.appendChild(k); d.appendChild(v);
    rowBox.appendChild(d);
  });

  function mkBtn(box, label, fn, cls) {
    var b = document.createElement('button');
    b.textContent = label;
    if (cls) b.className = cls;
    b.onclick = fn;
    box.appendChild(b);
    return b;
  }

  function fmtSec(ms) {
    if (typeof ms !== 'number') return '--';
    return (ms / 1000).toFixed(1) + 's';
  }

  var box = el('slBtns');
  mkBtn(box, '\u73B0\u5728\u7761', function () { get('/sleep/now').then(apply).catch(function () {}); });
  mkBtn(box, '\u73B0\u5728\u9192', function () { get('/sleep/wake').then(apply).catch(function () {}); });
  mkBtn(box, '\u5F3A\u5236\u72AF\u56F0', function () { get('/sleep/drowsy').then(apply).catch(function () {}); });
  var btnOn  = mkBtn(box, '\u5F00\u542F\u7761\u7720', function () { get('/sleep/set?on=1').then(apply).catch(function () {}); });
  var btnOff = mkBtn(box, '\u5173\u95ED\u7761\u7720', function () { get('/sleep/set?on=0').then(apply).catch(function () {}); });
  mkBtn(box, '\u91CD\u7F6E\u8BA1\u65F6', function () { get('/sleep/reset').then(apply).catch(function () {}); });
  var btnTest = mkBtn(box, '\u6D4B\u8BD5\u6863', function () {
    get('/sleep/test?on=' + (btnTest.classList.contains('on') ? '0' : '1')).then(apply).catch(function () {});
  });
  mkBtn(box, '\u771F\u5B9E\u65F6\u95F4', function () { get('/sleep/time?clear=1').then(apply).catch(function () {}); });
  mkBtn(box, '\u8BBE\u5907\u8C03\u8BD5\u5C4F', function () {
    get('/sleep/view').then(function () {
      if (window.toast) toast('\u8BBE\u5907\uFF1A\u7761\u7720\u8C03\u8BD5\u5C4F');
    }).catch(function () {});
  }, 'wide');

  // ── 模拟时间：验证跨午夜判断（22:59 白天 / 23:00 入夜 / 06:59 夜间 / 07:00 白天）──
  var TIMES = [['22:59', 22, 59], ['23:00', 23, 0], ['02:00', 2, 0],
               ['06:59', 6, 59], ['07:00', 7, 0]];
  var timeBox = el('slTimes');
  TIMES.forEach(function (t) {
    mkBtn(timeBox, t[0], function () {
      get('/sleep/time?h=' + t[1] + '&m=' + t[2]).then(apply).catch(function () {});
    });
  });

  function apply(j) {
    if (!j) return;

    var st = el('sl_state');
    if (st) {
      st.textContent = j.stateLabel || '--';
      if (j.sleeping) st.classList.add('sleep');
      else            st.classList.remove('sleep');
    }

    var m = {
      reason:   j.reasonLabel || '--',
      clock:    j.clock || '--:--',
      enabled:  j.enabled ? (j.testMode ? 'ON  TEST' : 'ON') : 'OFF',
      idle:     fmtSec(j.idle),
      drowsyIn: j.drowsyIn === 0 ? 'NOW' : fmtSec(j.drowsyIn),
      sleepIn:  j.sleepIn  === 0 ? 'NOW' : fmtSec(j.sleepIn),
      cooldown: j.cooldown === 0 ? '--' : fmtSec(j.cooldown)
    };
    for (var k in m) {
      var n = el('sl_' + k);
      if (n) n.textContent = m[k];
    }

    if (btnOn)     btnOn.classList.toggle('on',  !!j.enabled);
    if (btnOff)    btnOff.classList.toggle('off', !j.enabled);
    if (btnTest)   btnTest.classList.toggle('on', !!j.testMode);

    var chips = timeBox ? timeBox.getElementsByTagName('button') : [];
    var tt = j.testTime || '';
    for (var i = 0; i < chips.length; i++) {
      chips[i].classList.toggle('on', chips[i].textContent === tt);
    }
  }

  // 本次修复：原来这里是 `get('/sleep/list').then(apply).catch(function(){})`
  // —— 请求失败被完全吞掉，界面就永远停在初始的 "--"，也看不到任何原因。
  // 现在：① 明确检查 HTTP 状态；② 失败时把错误写进 STATE 行；③ 1.2 秒后自动重试。
  function refresh() {
    if (document.hidden) return;
    fetch('/sleep/list').then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    }).then(function (j) {
      apply(j);
    }).catch(function (e) {
      var st = el('sl_state');
      if (st) st.textContent = '\u8BFB\u53D6\u5931\u8D25 ' + (e && e.message ? e.message : '');
      setTimeout(refresh, 1200);
    });
  }

  // 首次刷新延后 400ms：次界面打开时 /enter 正在整屏重绘设备屏，
  // 错开一下避免和那次重绘抢同一瞬间。
  setTimeout(refresh, 400);
  // 整合新增：面板收起时不轮询（展开后 <=1.5s 自动刷新）
  setInterval(function () { if (slot.offsetParent) refresh(); }, 1500);
})();
)rawjs";
