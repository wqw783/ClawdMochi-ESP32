/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  emotion_ui.h — 情绪调试界面（独立前端资源）
 *
 *  与第一阶段同样的挂载方式：
 *      /emo.css   ← EMO_UI_CSS
 *      /emo.js    ← EMO_UI_JS
 *  主页面只需：<link rel="stylesheet" href="/emo.css">
 *             <div id="emoSlot"></div><script defer src="/emo.js"></script>
 *
 *  数值与名称全部向 /emo/list 拉取，emotion.cpp 的表是唯一出处。
 *  文件编码必须保持 UTF-8。
 * ═══════════════════════════════════════════════════════════════════════════
 */
#pragma once

static const char EMO_UI_CSS[] PROGMEM = R"rawcss(
#emoSlot{display:flex;flex-direction:column;align-items:center;gap:14px;width:100%}
.emocur{width:100%;max-width:390px;background:#141013;border:1.5px solid #2a2420;
  border-radius:10px;padding:10px 14px;display:flex;justify-content:space-between;
  align-items:center;font-family:'Courier New',monospace}
.emocur .lbl{font-size:10px;color:#6a6058;letter-spacing:2px}
.emocur .val{font-size:17px;font-weight:bold;color:#e0a63c;letter-spacing:1px}
.emovals{width:100%;max-width:390px;display:flex;flex-direction:column;gap:6px}
.evrow{display:flex;align-items:center;gap:8px;font-family:'Courier New',monospace;font-size:10px}
.evrow .n{width:58px;color:#8a8278;letter-spacing:1px;flex:none}
.evrow .t{flex:1;height:9px;background:#2e2a28;border-radius:3px;overflow:hidden}
.evrow .f{display:block;height:100%;background:#28b878;border-radius:3px;transition:width .25s}
.evrow .v{width:30px;text-align:right;color:#d8d4cc;font-weight:bold;flex:none}
.emobtns{display:grid;grid-template-columns:repeat(3,1fr);gap:7px;width:100%;max-width:390px}
.emobtns button{background:#252428;border:1.5px solid #38343a;border-radius:10px;color:#b8b4ac;
  font-family:'Courier New',monospace;font-size:11px;font-weight:bold;padding:11px 4px;
  cursor:pointer;transition:all .12s}
.emobtns button:active:not(:disabled){transform:scale(.94)}
.emobtns button:disabled{opacity:.3;cursor:default}
.emobtns button.wide{grid-column:1 / -1}
.emobtns button.warn{border-color:#7a3a2a;color:#c97a5a}
.emohint{width:100%;max-width:390px;font-size:10px;color:#6a6058;line-height:1.7;padding:0 2px}
)rawcss";

static const char EMO_UI_JS[] PROGMEM = R"rawjs(
(function () {
  'use strict';

  var slot = document.getElementById('emoSlot');
  if (!slot) return;

  slot.innerHTML =
    '<div class="sec">// emotion</div>' +
    '<div class="emocur"><span class="lbl">CURRENT EMOTION</span>' +
    '<span class="val" id="emoCur">--</span></div>' +
    '<div class="emovals" id="emoVals"></div>' +
    '<div class="sec">// emotion test</div>' +
    '<div class="emobtns" id="emoBtns"></div>' +
    '<div class="emobtns" id="emoBtns2"></div>' +
    '<div class="emohint" id="emoHint"></div>';

  var EV = [];            // [{key,label,base,step}]
  var locked = false;
  var timer = null;

  function el(id) { return document.getElementById(id); }
  function get(url) { return fetch(url).then(function (r) { return r.json(); }); }

  // [文字, 值索引, 增减量]  —— 值索引 -1 表示组合动作，-2 表示 Reset
  var ROW1 = [['+ \u5f00\u5fc3', 0, 15], ['+ \u751f\u6c14', 2, 20], ['+ \u96be\u8fc7', 3, 20]];
  var ROW2 = [['+ \u597d\u5947', 4, 20], ['+ \u5bb3\u7f9e', 6, 20], ['+ \u60ca\u5413', 5, 50]];
  var ROW3 = [['+ \u7cbe\u529b', 1, 25], ['- \u7cbe\u529b', 1, -25], ['\u5174\u594b', -1, 0]];

  function buildVals() {
    var g = el('emoVals');
    if (!g) return;
    g.innerHTML = '';
    for (var i = 0; i < EV.length; i++) {
      var r = document.createElement('div');
      r.className = 'evrow';
      r.dataset.i = i;
      var n = document.createElement('span'); n.className = 'n';
      var t = document.createElement('span'); t.className = 't';
      var f = document.createElement('span'); f.className = 'f';
      var v = document.createElement('span'); v.className = 'v';
      n.textContent = EV[i].label;
      t.appendChild(f);
      r.appendChild(n); r.appendChild(t); r.appendChild(v);
      g.appendChild(r);
    }
  }

  function mkBtn(box, label, fn, cls) {
    var b = document.createElement('button');
    b.textContent = label;
    if (cls) b.className = cls;
    b.onclick = fn;
    box.appendChild(b);
  }

  function buildBtns() {
    var b1 = el('emoBtns'), b2 = el('emoBtns2');
    if (!b1 || !b2) return;
    b1.innerHTML = ''; b2.innerHTML = '';

    function add(box, item) {
      mkBtn(box, item[0], function () {
        if (item[1] === -1) {                       // 兴奋 = 开心 + 精力
          get('/emo/add?v=0&d=15').then(function () {
            return get('/emo/add?v=1&d=20');
          }).then(apply).catch(function () {});
        } else {
          get('/emo/add?v=' + item[1] + '&d=' + item[2]).then(apply).catch(function () {});
        }
      });
    }

    ROW1.forEach(function (x) { add(b1, x); });
    ROW2.forEach(function (x) { add(b1, x); });
    ROW3.forEach(function (x) { add(b1, x); });

    mkBtn(b2, 'Reset \u5168\u90e8\u5f52\u96f6', function () {
      get('/emo/reset').then(apply).catch(function () {});
    }, 'warn');
    mkBtn(b2, '\u5728\u8bbe\u5907\u4e0a\u663e\u793a\u8c03\u8bd5\u5c4f', function () {
      get('/emo/view').then(function () {
        if (window.toast) toast('\u8BBE\u5907\uFF1A\u60C5\u7EEA\u8C03\u8BD5\u5C4F');
      }).catch(function () {});
    }, 'wide');
  }

  function paint() {
    var rows = document.querySelectorAll('.evrow');
    for (var i = 0; i < rows.length; i++) {
      var f = rows[i].querySelector('.f');
      var v = rows[i].querySelector('.v');
      if (!f || !v) continue;
      var n = +f.dataset.val || 0;
      f.style.width = n + '%';
      v.textContent = n;
    }
    var hn = el('emoHint');
    if (hn) {
      hn.textContent = locked
        ? '\u8868\u60c5\u5df2\u9501\u5b9a\uff1a\u60c5\u7eea\u4f1a\u6b63\u5e38\u53d8\u5316\uff0c\u4f46\u5c4f\u5e55\u4fdd\u6301\u7528\u6237\u6307\u5b9a\u7684\u8868\u60c5\u4e0d\u88ab\u8986\u76d6\u3002'
        : '\u60c5\u7eea\u6b63\u5e38\u9a71\u52a8\u8868\u60c5\u3002\u6539\u53d8\u60c5\u7eea\u540e\u7ea6 1.5 \u79d2\u5207\u6362\uff08\u5019\u9009\u7a33\u5b9a\u786e\u8ba4\uff09\u3002';
    }
  }

  function apply(j) {
    if (!j) return;
    if (j.key) {
      var c = el('emoCur');
      if (c) c.textContent = j.label + '  \u00b7  ' + j.key;
    }
    if (typeof j.locked === 'boolean') locked = j.locked;
    if (j.values && j.values.length) {
      var rows = document.querySelectorAll('.evrow');
      for (var i = 0; i < rows.length && i < j.values.length; i++) {
        var f = rows[i].querySelector('.f');
        if (f) f.dataset.val = j.values[i];
      }
    }
    paint();
  }

  function refresh() {
    if (document.hidden) return;
    get('/emo/list').then(apply).catch(function () {});
  }

  get('/emo/list').then(function (j) {
    if (j && j.keys) {
      for (var i = 0; i < j.keys.length; i++) {
        EV.push({ key: j.keys[i], label: j.labels[i], base: j.base[i], step: j.step[i] });
      }
    }
    buildVals();
    buildBtns();
    apply(j);
    // 整合新增：面板收起时不轮询（展开后 <=3s 自动刷新）
    timer = setInterval(function () { if (slot.offsetParent) refresh(); }, 3000);
  }).catch(function () {});
})();
)rawjs";
