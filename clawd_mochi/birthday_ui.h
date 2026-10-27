/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  birthday_ui.h — 生日 / 纪念日调试界面（独立前端资源）
 *
 *      /bday.css   ← BDAY_UI_CSS
 *      /bday.js    ← BDAY_UI_JS
 *
 *  主页面只需：<link rel="stylesheet" href="/bday.css">
 *             <div id="bdaySlot"></div><script defer src="/bday.js"></script>
 *
 *  这里全部是**开发测试按钮**（需求第二十二条）：
 *      添加 / 编辑 / 删除 / 启停
 *      [测试生日] [测试纪念日] [测试提前7天] [提前3天] [提前1天]
 *      [清除触发状态] [设置测试日期] [清除测试日期]
 *
 *  「设置测试日期」是验证跨年 / 闰年边界的关键：
 *      把「今天」挪到 2027-02-28，看 2/29 的生日是不是提前到那天触发。
 *
 *  文件编码必须保持 UTF-8（本文件里的 JS 全部用 \u 转义，纯 ASCII）。
 * ═══════════════════════════════════════════════════════════════════════════
 */
#pragma once

static const char BDAY_UI_CSS[] PROGMEM = R"rawcss(
#bdaySlot{display:flex;flex-direction:column;align-items:center;gap:14px;width:100%}
.bdrows{width:100%;max-width:390px;display:flex;flex-direction:column;gap:5px;
  font-family:'Courier New',monospace;font-size:11px}
.bdrow{display:flex;justify-content:space-between;background:#141013;
  border:1px solid #2a2420;border-radius:7px;padding:7px 11px}
.bdrow .k{color:#6a6058;letter-spacing:1px}
.bdrow .v{color:#7fb8e0;font-weight:bold}
.bdrow .v.ok{color:#28b878}
.bdrow .v.warn{color:#d8a24a}
.bdrow .v.bad{color:#e0553a}
.bdlist{width:100%;max-width:390px;display:flex;flex-direction:column;gap:8px}
.bdcard{background:#141013;border:1px solid #2a2420;border-left:3px solid #7fb8e0;
  border-radius:8px;padding:9px 11px;display:flex;flex-direction:column;gap:6px}
.bdcard.today{border-left-color:#d77757}
.bdcard.soon{border-left-color:#28b878}
.bdcard.passed{border-left-color:#3a3a3a;opacity:.7}
.bdcard.off{border-left-color:#3a3a3a;opacity:.5}
.bdhead{display:flex;justify-content:space-between;align-items:baseline;gap:8px}
.bdtitle{color:#e8e4dc;font-weight:bold;font-size:13px;
  font-family:'Courier New',monospace;word-break:break-all}
.bddays{font-weight:bold;font-size:13px;white-space:nowrap;color:#7fb8e0;
  font-family:'Courier New',monospace}
.bddays.today{color:#d77757}
.bddays.soon{color:#28b878}
.bddays.off{color:#6a6058}
.bdmeta{display:flex;justify-content:space-between;gap:8px;font-size:10px;color:#6a6058;
  font-family:'Courier New',monospace}
.bdbadge{border:1px solid #38343a;border-radius:5px;padding:1px 6px;color:#8a8278}
.bdbadge.today{color:#d77757;border-color:#d77757}
.bdbadge.soon{color:#28b878;border-color:#28b878}
.bdacts{display:grid;grid-template-columns:repeat(4,1fr);gap:5px}
.bdacts button{background:#252428;border:1.5px solid #38343a;border-radius:7px;color:#b8b4ac;
  font-family:'Courier New',monospace;font-size:10px;font-weight:bold;padding:7px 2px;
  cursor:pointer;transition:all .12s}
.bdacts button:active{transform:scale(.94)}
.bdacts button.hi{border-color:#7fb8e0;color:#7fb8e0}
.bdacts button.warn{border-color:#7a3a2a;color:#c97a5a}
.bdacts button.on{border-color:#28b878;color:#28b878;background:#0c1e12}
.bdform{width:100%;max-width:390px;display:flex;flex-direction:column;gap:6px}
.bdi{background:#0c1018;border:1.5px solid #2a2420;border-radius:8px;color:#e8e4dc;
  font-family:'Courier New',monospace;font-size:12px;padding:9px;outline:none}
.bdi::placeholder{color:#3a3630}
.bdn{flex:1;min-width:0;text-align:center;padding:9px 2px}
.bdrow3{display:flex;gap:5px}
.bdbtns{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;width:100%;max-width:390px}
.bdbtns button{background:#252428;border:1.5px solid #38343a;border-radius:9px;color:#b8b4ac;
  font-family:'Courier New',monospace;font-size:11px;font-weight:bold;padding:10px 4px;
  cursor:pointer;transition:all .12s}
.bdbtns button:active{transform:scale(.94)}
.bdbtns button.hi{border-color:#7fb8e0;color:#7fb8e0}
.bdbtns button.warn{border-color:#7a3a2a;color:#c97a5a}
.bdbtns button.wide{grid-column:1 / -1}
.bdhint{width:100%;max-width:390px;font-size:10px;color:#6a6058;line-height:1.7;padding:0 2px}
)rawcss";

static const char BDAY_UI_JS[] PROGMEM = R"rawjs(
(function () {
  'use strict';

  var slot = document.getElementById('bdaySlot');
  if (!slot) return;

  slot.innerHTML =
    '<div class="sec">//\u751F\u65E5 / \u7EAA\u5FF5\u65E5</div>' +
    '<div class="bdrows" id="bdRows"></div>' +
    '<div class="sec">//\u7279\u6B8A\u65E5\u671F</div>' +
    '<div class="bdlist" id="bdList"></div>' +
    '<div class="sec">//\u6DFB\u52A0 / \u7F16\u8F91</div>' +
    '<div class="bdform">' +
      '<select class="bdi" id="bdType">' +
        '<option value="0">\u751F\u65E5</option>' +
        '<option value="1">\u7EAA\u5FF5\u65E5</option>' +
        '<option value="2">\u7279\u6B8A\u65E5\u671F</option>' +
      '</select>' +
      '<input class="bdi" id="bdTitle" maxlength="23" ' +
        'placeholder="\u6807\u9898\uFF08\u4EC5 ASCII \u4F1A\u663E\u793A\u5728 240\u00D7240 \u5C4F\u5E55\uFF09">' +
      '<div class="bdrow3">' +
        '<input class="bdi bdn" id="bdMo" placeholder="\u6708" maxlength="2">' +
        '<input class="bdi bdn" id="bdD"  placeholder="\u65E5" maxlength="2">' +
        '<input class="bdi bdn" id="bdY"  placeholder="\u5E74" maxlength="4">' +
      '</div>' +
      '<div class="bdbtns">' +
        '<button id="bdSave" class="hi">\u6DFB\u52A0</button>' +
        '<button id="bdCancel" class="wide" style="display:none">' +
          '\u53D6\u6D88\u7F16\u8F91</button>' +
      '</div>' +
    '</div>' +
    '<div class="sec">//\u89E6\u53D1\u6D4B\u8BD5</div>' +
    '<div class="bdbtns" id="bdTest"></div>' +
    '<div class="sec">//\u6D4B\u8BD5\u65E5\u671F</div>' +
    '<div class="bdform">' +
      '<div class="bdrow3">' +
        '<input class="bdi bdn" id="tdY"  placeholder="\u5E74" maxlength="4">' +
        '<input class="bdi bdn" id="tdMo" placeholder="\u6708" maxlength="2">' +
        '<input class="bdi bdn" id="tdD"  placeholder="\u65E5" maxlength="2">' +
      '</div>' +
      '<div class="bdbtns">' +
        '<button id="tdSet" class="hi">\u8BBE\u7F6E\u6D4B\u8BD5\u65E5\u671F</button>' +
        '<button id="tdClr">\u6E05\u9664</button>' +
        '<button id="tdLeap">2027-02-28</button>' +
        '<button id="tdNy">2026-12-31</button>' +
      '</div>' +
    '</div>' +
    '<div class="bdhint">' +
      '\u63D0\u524D\u63D0\u9192\u6863\u4F4D\uFF1A7 \u5929\u524D / 3 \u5929\u524D / ' +
      '1 \u5929\u524D / \u5F53\u5929\uFF0C\u6BCF\u6863\u6BCF\u4E2A\u5468\u671F\u53EA\u54CD\u4E00\u6B21\u3002' +
    '</div>' +
    '<div class="bdhint">' +
      '2 \u6708 29 \u65E5\u5728\u5E73\u5E74\u63D0\u524D\u5230 2 \u6708 28 \u65E5\u3002' +
      '\u628A\u6D4B\u8BD5\u65E5\u671F\u8BBE\u6210 2027-02-28 \u53EF\u4EE5\u9A8C\u8BC1\u3002' +
    '</div>' +
    '<div class="bdhint">' +
      '\u5E86\u795D\u53EA\u5728\u8868\u60C5\u89C6\u56FE\u4E0B\u81EA\u52A8\u64AD\uFF1B' +
      '\u6B63\u5728\u7761\u89C9\u6216\u8868\u60C5\u88AB\u9501\u5B9A\u65F6\u4F1A\u6302\u8D77\uFF0C' +
      '\u7B49\u6761\u4EF6\u6EE1\u8DB3\u518D\u653E\u3002' +
    '</div>';

  var ROWS = [
    ['\u4ECA\u5929', 'today'], ['\u6765\u6E90', 'src'], ['\u6570\u91CF', 'count'],
    ['\u4E0B\u4E00\u4E2A', 'next'], ['\u5E86\u795D', 'cele'], ['\u4FDD\u5B58\u6570', 'saves'], ['\u5B58\u50A8', 'store']
  ];

  function el(id) { return document.getElementById(id); }
  function get(url) { return fetch(url).then(function (r) { return r.json(); }); }
  function pad(n) { return (n < 10 ? '0' : '') + n; }

  var rowBox = el('bdRows');
  ROWS.forEach(function (r) {
    var d = document.createElement('div');
    d.className = 'bdrow';
    var k = document.createElement('span'); k.className = 'k'; k.textContent = r[0];
    var v = document.createElement('span'); v.className = 'v'; v.id = 'bd_' + r[1];
    v.textContent = '--';
    d.appendChild(k); d.appendChild(v);
    rowBox.appendChild(d);
  });

  // ── 触发测试按钮 ──
  var testBox = el('bdTest');
  function mkBtn(box, label, fn, cls) {
    var b = document.createElement('button');
    b.textContent = label;
    if (cls) b.className = cls;
    b.onclick = fn;
    box.appendChild(b);
    return b;
  }
  mkBtn(testBox, '\u6D4B\u8BD5\u751F\u65E5', function () {
    get('/bday/test?kind=birthday').then(apply).catch(function () {});
  }, 'hi');
  mkBtn(testBox, '\u6D4B\u8BD5\u7EAA\u5FF5\u65E5', function () {
    get('/bday/test?kind=anniv').then(apply).catch(function () {});
  }, 'hi');
  mkBtn(testBox, '\u6D4B\u8BD5\u63D0\u524D7\u5929', function () {
    get('/bday/test?kind=d7').then(apply).catch(function () {});
  });
  mkBtn(testBox, '\u63D0\u524D3\u5929', function () {
    get('/bday/test?kind=d3').then(apply).catch(function () {});
  });
  mkBtn(testBox, '\u63D0\u524D1\u5929', function () {
    get('/bday/test?kind=d1').then(apply).catch(function () {});
  });
  mkBtn(testBox, '\u6E05\u9664\u89E6\u53D1\u72B6\u6001', function () {
    get('/bday/reset').then(apply).catch(function () {});
  }, 'warn');
  mkBtn(testBox, '\u6E05\u7A7A\u5168\u90E8', function () {
    if (window.confirm && !confirm('\u6E05\u7A7A\u5168\u90E8\u7279\u6B8A\u65E5\u671F\uFF1F')) return;
    get('/bday/clear').then(apply).catch(function () {});
  }, 'warn');
  mkBtn(testBox, '\u8BBE\u5907\u8C03\u8BD5\u5C4F', function () {
    get('/bday/view').then(function () {
      if (window.toast) toast('\u8BBE\u5907\uFF1A\u7279\u6B8A\u65E5\u671F');
    }).catch(function () {});
  }, 'wide');

  // ── 测试日期 ──
  el('tdSet').onclick = function () {
    get('/bday/date?y=' + el('tdY').value + '&mo=' + el('tdMo').value +
        '&d=' + el('tdD').value).then(apply).catch(function () {});
  };
  el('tdClr').onclick = function () {
    get('/bday/date?clear=1').then(apply).catch(function () {});
  };
  function quickDate(y, mo, d) {
    el('tdY').value = y; el('tdMo').value = mo; el('tdD').value = d;
    get('/bday/date?y=' + y + '&mo=' + mo + '&d=' + d).then(apply).catch(function () {});
  }
  el('tdLeap').onclick = function () { quickDate(2027, 2, 28); };
  el('tdNy').onclick   = function () { quickDate(2026, 12, 31); };

  // ── 显示格式化 ──
  function fmtDays(du, enabled) {
    if (!enabled) return '\u5DF2\u505C\u7528';
    if (du < 0) return '--';
    if (du === 0) return '\u5C31\u662F\u4ECA\u5929';
    return '\u8FD8\u6709 ' + du + ' \u5929';
  }

  var TYPE_LABEL = ['\u751F\u65E5', '\u7EAA\u5FF5\u65E5', '\u7279\u6B8A\u65E5\u671F'];
  var TYPE_GLYPH = ['\uD83C\uDF82', '\u2764\uFE0F', '\uD83C\uDF89'];

  var editingId = 0;

  function act(label, fn, cls) {
    var b = document.createElement('button');
    b.textContent = label;
    if (cls) b.className = cls;
    b.onclick = fn;
    return b;
  }

  function renderList(j) {
    var box = el('bdList');
    box.innerHTML = '';
    if (!j.days || !j.days.length) {
      var e = document.createElement('div');
      e.className = 'bdhint';
      e.textContent = '\u8FD8\u6CA1\u6709\u7279\u6B8A\u65E5\u671F\u3002\u4E0B\u9762\u6DFB\u4E00\u4E2A\u3002';
      box.appendChild(e);
      return;
    }

    j.days.forEach(function (sd) {
      var cc = !sd.enabled ? 'off'
             : (sd.state === 'today' ? 'today'
             : (sd.state === 'upcoming' ? 'soon'
             : (sd.state === 'passed' ? 'passed' : '')));
      var c = document.createElement('div');
      c.className = 'bdcard' + (cc ? ' ' + cc : '');

      var head = document.createElement('div');
      head.className = 'bdhead';
      var t = document.createElement('span');
      t.className = 'bdtitle';
      t.textContent = TYPE_GLYPH[sd.type] + ' ' + sd.title;
      var dd = document.createElement('span');
      dd.className = 'bddays' + (cc ? ' ' + cc : '');
      dd.textContent = fmtDays(sd.daysUntil, sd.enabled);
      head.appendChild(t); head.appendChild(dd);
      c.appendChild(head);

      var meta = document.createElement('div');
      meta.className = 'bdmeta';
      var m1 = document.createElement('span');
      m1.textContent = sd.month + '\u6708' + sd.day + '\u65E5' +
                       (sd.years > 0
                          ? '   ' + (sd.type === 0 ? '\u5E74\u9F84 ' : '\u7B2C ') + sd.years +
                            (sd.type === 0 ? '' : ' \u5E74')
                          : (sd.year > 0 ? '   ' + sd.year + ' \u8D77' : '')) +
                       '   #' + sd.id;
      var m2 = document.createElement('span');
      m2.className = 'bdbadge' + (cc === 'today' ? ' today' : cc === 'soon' ? ' soon' : '');
      m2.textContent = sd.enabled ? sd.stateLabel : '\u5DF2\u505C\u7528';
      meta.appendChild(m1); meta.appendChild(m2);
      c.appendChild(meta);

      var a1 = document.createElement('div');
      a1.className = 'bdacts';
      a1.appendChild(act('\u7F16\u8F91', function () { startEdit(sd); }));
      a1.appendChild(act('\u5220\u9664', function () {
        get('/bday/del?id=' + sd.id).then(apply).catch(function () {});
      }, 'warn'));
      a1.appendChild(act(sd.enabled ? '\u505C\u7528' : '\u542F\u7528', function () {
        get('/bday/en?id=' + sd.id + '&on=' + (sd.enabled ? 0 : 1)).then(apply).catch(function () {});
      }, sd.enabled ? '' : 'on'));
      a1.appendChild(act('\u9884\u89C8', function () {
        get('/bday/preview?id=' + sd.id).then(apply).catch(function () {});
      }, 'hi'));
      c.appendChild(a1);

      box.appendChild(c);
    });
  }

  function startEdit(sd) {
    editingId = sd.id;
    el('bdTitle').value = sd.title;
    el('bdType').value = sd.type;
    el('bdMo').value = sd.month;
    el('bdD').value = sd.day;
    el('bdY').value = sd.year || '';
    el('bdSave').textContent = '\u4FDD\u5B58\u4FEE\u6539';
    el('bdCancel').style.display = '';
  }

  function stopEdit() {
    editingId = 0;
    el('bdSave').textContent = '\u6DFB\u52A0';
    el('bdCancel').style.display = 'none';
  }

  el('bdCancel').onclick = function () {
    stopEdit();
    el('bdTitle').value = '';
  };

  el('bdSave').onclick = function () {
    var q = 'type=' + el('bdType').value +
            '&title=' + encodeURIComponent(el('bdTitle').value) +
            '&mo=' + el('bdMo').value + '&d=' + el('bdD').value +
            '&y=' + (el('bdY').value || 0);
    var url = editingId ? ('/bday/upd?id=' + editingId + '&' + q)
                        : ('/bday/add?' + q);
    get(url).then(function (r) {
      stopEdit();
      el('bdTitle').value = '';
      apply(r);
    }).catch(function () {});
  };

  // ── 状态行更新 ──
  function apply(j) {
    if (!j) return;
    if (j.err && window.toast) { toast('\u751F\u65E5\uFF1A' + j.err); }

    function set(k, v, cls) {
      var n = el('bd_' + k);
      if (!n) return;
      n.textContent = v;
      n.className = 'v' + (cls ? ' ' + cls : '');
    }

    set('today', j.today || '--', j.timeReady ? 'ok' : 'warn');
    set('src',   j.testDate ? '\u6D4B\u8BD5' : (j.timeReady ? '\u771F\u5B9E' : '\u65E0\u65F6\u95F4'),
                 j.testDate ? 'warn' : (j.timeReady ? 'ok' : 'warn'));
    set('count', j.count + ' / ' + j.max, j.count >= j.max ? 'warn' : '');
    set('next',  j.nextTitle ? (j.nextTitle + '  ' + j.nextDays + '\u5929') : '--',
                 (j.nextDays === 0) ? 'warn' : '');
    set('cele',  j.celePhase || 'idle',
                 (j.celePhase && j.celePhase !== 'idle') ? 'ok' : '');
    set('saves', String(j.saveCount));
    set('store', j.storage ? 'NVS \u6B63\u5E38' : '\u5F02\u5E38', j.storage ? 'ok' : 'warn');

    renderList(j);
  }

  function refresh() {
    if (document.hidden) return;
    get('/bday/list').then(apply).catch(function () {});
  }

  // 默认填「今天」
  (function seed() {
    var d = new Date();
    el('bdMo').value = d.getMonth() + 1;
    el('bdD').value  = d.getDate();
    el('bdY').value  = d.getFullYear();
    el('tdY').value  = d.getFullYear();
    el('tdMo').value = d.getMonth() + 1;
    el('tdD').value  = d.getDate();
  })();

  refresh();
  // 整合新增：面板收起（display:none）时不轮询，省掉 ESP32 上的空转请求；
  // 面板一展开，下一拍（<=1.5s）自动刷新
  setInterval(function () { if (slot.offsetParent) refresh(); }, 1500);
})();
)rawjs";
