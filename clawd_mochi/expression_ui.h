/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  expression_ui.h — 表情管理界面（独立前端资源）
 *
 *  以两个独立资源的形式挂到主控制页上：
 *      /expr.css   ← EXPR_UI_CSS
 *      /expr.js    ← EXPR_UI_JS
 *  主页面只需两行：<link rel="stylesheet" href="/expr.css"> 和
 *  <div id="exprSlot"></div> + <script defer src="/expr.js"></script>
 *
 *  这样做的好处：
 *    · 完全不碰主页面那段 33KB 的 PROGMEM 字符串的发送方式（沿用原有 send_P）
 *    · ExpressionUI 是真正独立的模块，改界面不用动固件逻辑
 *    · 表情清单运行时向 /expr/list 拉取，EXPR_DEFS 是唯一出处
 *
 *  文件编码必须保持 UTF-8。
 * ═══════════════════════════════════════════════════════════════════════════
 */
#pragma once

static const char EXPR_UI_CSS[] PROGMEM = R"rawcss(
#exprSlot{display:flex;flex-direction:column;align-items:center;gap:14px;width:100%}
.egrid{display:grid;grid-template-columns:repeat(auto-fill,minmax(64px,1fr));gap:7px;
  width:100%;max-width:390px}
.ebtn{background:#252428;border:1.5px solid #38343a;border-radius:11px;color:#d8d4cc;
  font-family:'Courier New',monospace;padding:9px 2px 7px;cursor:pointer;
  text-align:center;transition:all .12s;user-select:none}
.ebtn:active:not(:disabled){transform:scale(.92)}
.ebtn:disabled{opacity:.3;cursor:default}
.ebtn .eg{font-size:21px;display:block;line-height:1.15}
.ebtn .en{display:block;font-size:11px;font-weight:bold;color:#a8a49c;margin-top:2px}
.ebtn.active{border-color:#c96a3e;background:#201408}
.ebtn.active .en{color:#e8e4dc}
.mgrid{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;width:100%;max-width:390px}
.mbtn{background:#252428;border:1.5px solid #38343a;border-radius:9px;color:#b8b4ac;
  font-family:'Courier New',monospace;font-size:10px;font-weight:bold;padding:9px 2px;
  cursor:pointer;text-align:center;transition:all .12s}
.mbtn:active:not(:disabled){transform:scale(.94)}
.mbtn:disabled{opacity:.3;cursor:default}
.mbtn.active{border-color:#d9a441;color:#e0a63c;background:#1d1505}
.ehint{width:100%;max-width:390px;font-size:10px;color:#6a6058;line-height:1.7;padding:0 2px}
)rawcss";

static const char EXPR_UI_JS[] PROGMEM = R"rawjs(
(function () {
  'use strict';

  var slot = document.getElementById('exprSlot');
  if (!slot) return;

  slot.innerHTML =
    '<div class="sec">// expressions</div>' +
    '<div class="egrid" id="egrid"></div>' +
    '<div class="sec">// expression lock</div>' +
    '<div class="ctrl"><button class="cbtn" id="exprLock"></button></div>' +
    '<div class="ehint" id="exprHint"></div>' +
    '<div class="sec">// motion</div>' +
    '<div class="mgrid" id="mgrid"></div>' +
    '<div class="ehint">动作与表情相互独立，可任意组合（例如 开心 + 跳动）。</div>';

  var list = [], curExpr = 0, curAnim = 0, locked = false;
  var MOTIONS = [['none', '静止'], ['blink', '眨眼'], ['bounce', '跳动'],
                 ['shake', '晃动'], ['breath', '呼吸'], ['tremble', '颤抖'],
                 ['sleep_breath', '睡眠呼吸']];   // 整合补全：第 7 种动画（ANIM_SLEEP_BREATH）

  function el(id) { return document.getElementById(id); }
  function get(url) { return fetch(url).then(function (r) { return r.json(); }); }

  function paint() {
    var i;
    var bs = document.querySelectorAll('.ebtn');
    for (i = 0; i < bs.length; i++) {
      bs[i].classList.toggle('active', +bs[i].dataset.id === curExpr);
    }
    var ms = document.querySelectorAll('.mbtn');
    for (i = 0; i < ms.length; i++) {
      ms[i].classList.toggle('active', +ms[i].dataset.v === curAnim);
    }
    var lk = el('exprLock');
    if (lk) {
      lk.textContent = locked
        ? '\uD83D\uDD13 \u89E3\u9501\u8868\u60C5'
        : '\uD83D\uDD12 \u9501\u5B9A\u8868\u60C5';
      lk.classList.toggle('on', locked);
    }
    var hn = el('exprHint');
    if (hn) {
      hn.textContent = locked
        ? '已锁定：自动表情请求（未来的情绪 / 睡眠 / 随机互动）无法覆盖当前表情；手动点击仍可切换。'
        : '未锁定：任何来源都可以切换表情。';
    }
  }

  function buildGrid() {
    var g = el('egrid');
    if (!g) return;
    g.innerHTML = '';
    for (var i = 0; i < list.length; i++) {
      (function (d) {
        var b = document.createElement('button');
        b.className = 'ebtn';
        b.dataset.id = d.id;
        var e1 = document.createElement('span');
        e1.className = 'eg';
        e1.textContent = d.glyph;
        var e2 = document.createElement('span');
        e2.className = 'en';
        e2.textContent = d.label;
        b.appendChild(e1);
        b.appendChild(e2);
        b.onclick = function () { pick(d.id); };
        g.appendChild(b);
      })(list[i]);
    }
  }

  function buildMotion() {
    var g = el('mgrid');
    if (!g) return;
    g.innerHTML = '';
    for (var i = 0; i < MOTIONS.length; i++) {
      (function (m, idx) {
        var b = document.createElement('button');
        b.className = 'mbtn';
        b.dataset.v = idx;
        b.textContent = m[1];
        b.onclick = function () { setMotion(idx); };
        g.appendChild(b);
      })(MOTIONS[i], i);
    }
  }

  function apply(j) {
    if (!j) return;
    if (typeof j.expr === 'number')    curExpr = j.expr;
    if (typeof j.anim === 'number')    curAnim = j.anim;
    if (typeof j.locked === 'boolean') locked  = j.locked;
    paint();
  }

  function pick(id) {
    get('/expr?id=' + id).then(apply).catch(function () {
      if (window.toast) toast('no connection', false);
    });
  }

  function setMotion(v) {
    get('/expr/anim?v=' + v).then(apply).catch(function () {});
  }

  function toggleLock() {
    get('/expr/lock?on=' + (locked ? 0 : 1)).then(apply).catch(function () {});
  }

  el('exprLock').onclick = toggleLock;
  buildMotion();
  get('/expr/list').then(function (j) {
    list = j || [];
    buildGrid();
    paint();
  }).catch(function () {});
  get('/state').then(apply).catch(function () {});
})();
)rawjs";
