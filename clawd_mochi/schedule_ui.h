/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  schedule_ui.h — 日程 / 倒计时调试界面（独立前端资源）
 *
 *      /sched.css   ← SCH_UI_CSS
 *      /sched.js    ← SCH_UI_JS
 *
 *  主页面只需：<link rel="stylesheet" href="/sched.css">
 *             <div id="schedSlot"></div><script defer src="/sched.js"></script>
 *
 *  这里全部是**开发测试按钮**：
 *      添加 / 编辑 / 删除 / 启停 / 标记完成
 *      + 快捷添加 10s / 1m / 1h / 1d / 31d / 365d / 2028-02-29（闰年）
 *      + 每个事件带 T-1天 / T-1时 / T-10分 / T-1分 的「时间快进」
 *        —— 这是把需求第二十九条那 6 个边界用例在真机上跑出来的关键：
 *           点一下就把测试时间挪到目标前 N 分钟，不用真的等。
 *
 *  文件编码必须保持 UTF-8（本文件里的 JS 全部用 \u 转义，纯 ASCII）。
 * ═══════════════════════════════════════════════════════════════════════════
 */
#pragma once

static const char SCH_UI_CSS[] PROGMEM = R"rawcss(
#schedSlot{display:flex;flex-direction:column;align-items:center;gap:14px;width:100%}
.scrows{width:100%;max-width:390px;display:flex;flex-direction:column;gap:5px;
  font-family:'Courier New',monospace;font-size:11px}
.scrow{display:flex;justify-content:space-between;background:#141013;
  border:1px solid #2a2420;border-radius:7px;padding:7px 11px}
.scrow .k{color:#6a6058;letter-spacing:1px}
.scrow .v{color:#7fb8e0;font-weight:bold}
.scrow .v.warn{color:#d8a24a}
.scrow .v.ok{color:#28b878}
.sclist{width:100%;max-width:390px;display:flex;flex-direction:column;gap:8px}
.sccard{background:#141013;border:1px solid #2a2420;border-left:3px solid #7fb8e0;
  border-radius:8px;padding:9px 11px;display:flex;flex-direction:column;gap:6px}
.sccard.soon{border-left-color:#28b878}
.sccard.urgent{border-left-color:#d8a24a}
.sccard.due{border-left-color:#e0553a}
.sccard.done{border-left-color:#3a3a3a;opacity:.55}
.sccard.off{border-left-color:#3a3a3a;opacity:.55}
.schead{display:flex;justify-content:space-between;align-items:baseline;gap:8px}
.sctitle{color:#e8e4dc;font-weight:bold;font-size:13px;
  font-family:'Courier New',monospace;word-break:break-all}
.sccd{color:#28b878;font-weight:bold;font-size:13px;white-space:nowrap;
  font-family:'Courier New',monospace}
.sccd.urgent{color:#d8a24a}
.sccd.due{color:#e0553a}
.sccd.done{color:#6a6058}
.scmeta{display:flex;justify-content:space-between;gap:8px;font-size:10px;color:#6a6058;
  font-family:'Courier New',monospace}
.scbadge{border:1px solid #38343a;border-radius:5px;padding:1px 6px;color:#8a8278}
.scbadge.soon{color:#28b878;border-color:#28b878}
.scbadge.urgent{color:#d8a24a;border-color:#d8a24a}
.scbadge.due{color:#e0553a;border-color:#e0553a}
.scacts{display:grid;grid-template-columns:repeat(4,1fr);gap:5px}
.scacts button{background:#252428;border:1.5px solid #38343a;border-radius:7px;color:#b8b4ac;
  font-family:'Courier New',monospace;font-size:10px;font-weight:bold;padding:7px 2px;
  cursor:pointer;transition:all .12s}
.scacts button:active{transform:scale(.94)}
.scacts button.hi{border-color:#7fb8e0;color:#7fb8e0}
.scacts button.warn{border-color:#7a3a2a;color:#c97a5a}
.scacts button.on{border-color:#28b878;color:#28b878;background:#0c1e12}
.scform{width:100%;max-width:390px;display:flex;flex-direction:column;gap:6px}
.sci,.scn{background:#0c1018;border:1.5px solid #2a2420;border-radius:8px;color:#e8e4dc;
  font-family:'Courier New',monospace;font-size:12px;padding:9px;outline:none}
.sci::placeholder,.scn::placeholder{color:#3a3630}
.scn{flex:1;min-width:0;text-align:center;padding:9px 2px}
.scrow5{display:flex;gap:5px}
.scbtns{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;width:100%;max-width:390px}
.scbtns button{background:#252428;border:1.5px solid #38343a;border-radius:9px;color:#b8b4ac;
  font-family:'Courier New',monospace;font-size:11px;font-weight:bold;padding:10px 4px;
  cursor:pointer;transition:all .12s}
.scbtns button:active{transform:scale(.94)}
.scbtns button.hi{border-color:#7fb8e0;color:#7fb8e0}
.scbtns button.warn{border-color:#7a3a2a;color:#c97a5a}
.scbtns button.wide{grid-column:1 / -1}
.scquick{display:grid;grid-template-columns:repeat(4,1fr);gap:5px;width:100%;max-width:390px}
.scquick button{background:#141013;border:1.5px solid #2a2420;border-radius:8px;color:#8a8278;
  font-family:'Courier New',monospace;font-size:10px;font-weight:bold;padding:8px 2px;
  cursor:pointer;transition:all .12s}
.scquick button:active{transform:scale(.94)}
.schint{width:100%;max-width:390px;font-size:10px;color:#6a6058;line-height:1.7;padding:0 2px}
)rawcss";

static const char SCH_UI_JS[] PROGMEM = R"rawjs(
(function () {
  'use strict';

  var slot = document.getElementById('schedSlot');
  if (!slot) return;

  slot.innerHTML =
    '<div class="sec">//\u65E5\u7A0B</div>' +
    '<div class="scrows" id="scRows"></div>' +
    '<div class="sec">//\u4E8B\u4EF6</div>' +
    '<div class="sclist" id="scList"></div>' +
    '<div class="sec">//\u6DFB\u52A0 / \u7F16\u8F91</div>' +
    '<div class="scform">' +
      '<input class="sci" id="scTitle" maxlength="23" ' +
        'placeholder="\u6807\u9898\uFF08\u4EC5 ASCII \u4F1A\u663E\u793A\u5728 240\u00D7240 \u5C4F\u5E55\uFF09">' +
      '<select class="sci" id="scType"></select>' +
      '<div class="scrow5">' +
        '<input class="scn" id="scY"  placeholder="\u5E74" maxlength="4">' +
        '<input class="scn" id="scMo" placeholder="\u6708" maxlength="2">' +
        '<input class="scn" id="scD"  placeholder="\u65E5" maxlength="2">' +
        '<input class="scn" id="scH"  placeholder="\u65F6" maxlength="2">' +
        '<input class="scn" id="scMi" placeholder="\u5206" maxlength="2">' +
      '</div>' +
      '<div class="scbtns">' +
        '<button id="scSave" class="hi">\u6DFB\u52A0</button>' +
        '<button id="scCancel" class="wide" style="display:none">\u53D6\u6D88\u7F16\u8F91</button>' +
      '</div>' +
    '</div>' +
    '<div class="sec">//\u5FEB\u6377\u6DFB\u52A0</div>' +
    '<div class="scquick" id="scQuick"></div>' +
    '<div class="sec">//\u6D4B\u8BD5</div>' +
    '<div class="scbtns" id="scTest"></div>' +
    '<div class="schint">' +
      '\u5012\u8BA1\u65F6 = \u76EE\u6807\u65F6\u95F4 \u2212 \u73B0\u5728\uFF0C' +
      '\u7EAF\u6574\u6570\u79D2\u5DEE\uFF0C\u6240\u4EE5\u8DE8\u5929 / \u8DE8\u6708 / ' +
      '\u8DE8\u5E74 / \u95F0\u5E74\u5168\u90E8\u81EA\u52A8\u6B63\u786E\u3002' +
    '</div>' +
    '<div class="schint">' +
      '\u6BCF\u4E2A\u4E8B\u4EF6\u4E0B\u9762\u7684 T-1\u5929 / T-1\u65F6 / T-10\u5206 / T-1\u5206 ' +
      '\u662F\u201C\u65F6\u95F4\u5FEB\u8FDB\u201D\uFF1A\u628A\u6D4B\u8BD5\u65F6\u95F4\u632A\u5230' +
      '\u76EE\u6807\u524D N \u5206\u949F\uFF0C\u4E0D\u7528\u771F\u7684\u7B49\u3002' +
    '</div>' +
    '<div class="schint">' +
      '\u26A0 \u8BBE\u5907\u7AEF 240\u00D7240 \u53EA\u6709 ASCII \u5B57\u4F53\uFF0C' +
      '\u4E2D\u6587\u6807\u9898\u5728\u5C4F\u5E55\u4E0A\u4F1A\u9000\u56DE\u7C7B\u578B\u6807\u7B7E' +
      '\uFF08\u5982 [EXAM]\uFF09\u3002\u5B8C\u6574\u4E2D\u6587\u5728\u8FD9\u91CC\u770B\u3002' +
    '</div>';

  var TYPES = [
    ['general', '\u901A\u7528'],
    ['exam', '\u8003\u8BD5'],
    ['competition', '\u6BD4\u8D5B'],
    ['task', '\u4EFB\u52A1'],
    ['birthday', '\u751F\u65E5'],
    ['anniversary', '\u7EAA\u5FF5\u65E5']
  ];

  var ROWS = [
    ['\u73B0\u5728', 'now'], ['\u540C\u6B65', 'sync'], ['\u6570\u91CF', 'count'],
    ['\u4E0B\u4E00\u4E2A', 'next'], ['\u4FDD\u5B58\u6570', 'saves'], ['\u5B58\u50A8', 'store']
  ];

  function el(id) { return document.getElementById(id); }
  function get(url) { return fetch(url).then(function (r) { return r.json(); }); }
  function pad(n) { return (n < 10 ? '0' : '') + n; }

  // ── 状态行 ──
  var rowBox = el('scRows');
  ROWS.forEach(function (r) {
    var d = document.createElement('div');
    d.className = 'scrow';
    var k = document.createElement('span'); k.className = 'k'; k.textContent = r[0];
    var v = document.createElement('span'); v.className = 'v'; v.id = 'sc_' + r[1];
    v.textContent = '--';
    d.appendChild(k); d.appendChild(v);
    rowBox.appendChild(d);
  });

  // ── 类型下拉 ──
  var typeSel = el('scType');
  TYPES.forEach(function (t) {
    var o = document.createElement('option');
    o.value = t[0]; o.textContent = t[1];
    typeSel.appendChild(o);
  });

  // ── 快捷添加 ──
  //  label 走 ASCII，方便一眼看出单位；闰年那条用绝对日期
  var QUICK = [
    ['+10s',      'in', 10],
    ['+1m',       'in', 60],
    ['+1h',       'in', 3600],
    ['+1d',       'in', 86400],
    ['+31d',      'in', 2678400],
    ['+365d',     'in', 31536000],
    ['2028-02-29','ymd', [2028, 2, 29, 0, 0]],
    ['2027-01-01','ymd', [2027, 1, 1, 0, 0]]
  ];
  var quickBox = el('scQuick');
  QUICK.forEach(function (q) {
    var b = document.createElement('button');
    b.textContent = q[0];
    b.onclick = function () {
      var t = el('scTitle').value || q[0];
      var url;
      if (q[1] === 'in') {
        url = '/sched/add?in=' + q[2] + '&title=' + encodeURIComponent(t) +
              '&type=' + typeSel.value;
      } else {
        var a = q[2];
        url = '/sched/add?y=' + a[0] + '&mo=' + a[1] + '&d=' + a[2] +
              '&h=' + a[3] + '&mi=' + a[4] + '&title=' + encodeURIComponent(t) +
              '&type=' + typeSel.value;
      }
      get(url).then(apply).catch(function () {});
    };
    quickBox.appendChild(b);
  });

  // ── 测试按钮 ──
  var testBox = el('scTest');
  function mkBtn(box, label, fn, cls) {
    var b = document.createElement('button');
    b.textContent = label;
    if (cls) b.className = cls;
    b.onclick = fn;
    box.appendChild(b);
    return b;
  }
  mkBtn(testBox, '\u73B0\u5728', function () { get('/sched/time?now=1').then(apply).catch(function () {}); });
  mkBtn(testBox, '+10\u5206', function () { get('/sched/time?add=600').then(apply).catch(function () {}); });
  mkBtn(testBox, '+1\u65F6', function () { get('/sched/time?add=3600').then(apply).catch(function () {}); });
  mkBtn(testBox, '+1\u5929', function () { get('/sched/time?add=86400').then(apply).catch(function () {}); });
  mkBtn(testBox, '\u6E05\u6D4B\u8BD5\u65F6\u95F4', function () { get('/sched/time?clear=1').then(apply).catch(function () {}); });
  mkBtn(testBox, '\u91CD\u7F6E\u63D0\u9192\u4F4D', function () { get('/sched/alerts').then(apply).catch(function () {}); });
  mkBtn(testBox, '\u6E05\u7A7A\u5168\u90E8', function () {
    if (window.confirm && !confirm('\u6E05\u7A7A\u5168\u90E8\u4E8B\u4EF6\uFF1F')) return;
    get('/sched/clear').then(apply).catch(function () {});
  }, 'warn');
  mkBtn(testBox, '\u5217\u8868\u5C4F', function () { get('/sched/view?m=0').catch(function () {}); });
  mkBtn(testBox, '\u8BE6\u60C5\u5C4F', function () { get('/sched/view?m=1').catch(function () {}); });
  mkBtn(testBox, '\u8BBE\u5907\u8C03\u8BD5\u5C4F', function () {
    get('/sched/view').then(function () {
      if (window.toast) toast('\u8BBE\u5907\uFF1A\u65E5\u7A0B');
    }).catch(function () {});
  }, 'wide');

  // ── 倒计时格式化（需求第三十条）──
  function fmt(remain) {
    if (remain <= 0) return '\u5DF2\u5230\u671F';
    var d = Math.floor(remain / 86400);
    var h = Math.floor((remain % 86400) / 3600);
    var m = Math.floor((remain % 3600) / 60);
    var s = remain % 60;
    if (d > 0) return d + '\u5929 ' + h + '\u5C0F\u65F6';
    if (h > 0) return h + '\u5C0F\u65F6 ' + m + '\u5206';
    return m + '\u5206 ' + s + '\u79D2';
  }

  // 目标时刻：localEpoch 已经是「本地 epoch」，所以按 UTC 取值
  function fmtTarget(e) {
    if (!e) return '--';
    var d = new Date(e * 1000);
    return d.getUTCFullYear() + '-' + pad(d.getUTCMonth() + 1) + '-' + pad(d.getUTCDate()) +
           ' ' + pad(d.getUTCHours()) + ':' + pad(d.getUTCMinutes());
  }

  function cardClass(ev) {
    if (!ev.enabled || ev.done) return ev.done ? 'done' : 'off';
    if (ev.state === 'completed') return 'done';
    if (ev.state === 'due') return 'due';
    if (ev.urgent) return 'urgent';
    if (ev.state === 'due_soon') return 'soon';
    return '';
  }

  var editingId = 0;

  function act(label, fn, cls) {
    var b = document.createElement('button');
    b.textContent = label;
    if (cls) b.className = cls;
    b.onclick = fn;
    return b;
  }

  function renderList(j) {
    var box = el('scList');
    box.innerHTML = '';
    if (!j.events || !j.events.length) {
      var e = document.createElement('div');
      e.className = 'schint';
      e.textContent = '\u8FD8\u6CA1\u6709\u4E8B\u4EF6\u3002\u4E0B\u9762\u6DFB\u4E00\u4E2A\u3002';
      box.appendChild(e);
      return;
    }

    j.events.forEach(function (ev) {
      var cc = cardClass(ev);
      var c = document.createElement('div');
      c.className = 'sccard' + (cc ? ' ' + cc : '');

      var head = document.createElement('div');
      head.className = 'schead';
      var t = document.createElement('span');
      t.className = 'sctitle';
      t.textContent = '[' + ev.typeLabel + '] ' + ev.title;
      var cd = document.createElement('span');
      cd.className = 'sccd' + (cc === 'urgent' ? ' urgent' : cc === 'due' ? ' due'
                              : cc === 'done' ? ' done' : '');
      cd.textContent = ev.enabled ? fmt(ev.remain) : '\u5DF2\u505C\u7528';
      head.appendChild(t); head.appendChild(cd);
      c.appendChild(head);

      var meta = document.createElement('div');
      meta.className = 'scmeta';
      var m1 = document.createElement('span');
      m1.textContent = fmtTarget(ev.target) + '   #' + ev.id;
      var m2 = document.createElement('span');
      m2.className = 'scbadge' + (cc === 'urgent' ? ' urgent' : cc === 'due' ? ' due'
                                 : cc === 'soon' ? ' soon' : '');
      m2.textContent = ev.enabled ? ev.stateLabel : '\u5DF2\u505C\u7528';
      meta.appendChild(m1); meta.appendChild(m2);
      c.appendChild(meta);

      var a1 = document.createElement('div');
      a1.className = 'scacts';
      a1.appendChild(act('\u7F16\u8F91', function () { startEdit(ev); }));
      a1.appendChild(act('\u5220\u9664', function () {
        get('/sched/del?id=' + ev.id).then(apply).catch(function () {});
      }, 'warn'));
      a1.appendChild(act(ev.enabled ? '\u505C\u7528' : '\u542F\u7528', function () {
        get('/sched/en?id=' + ev.id + '&on=' + (ev.enabled ? 0 : 1)).then(apply).catch(function () {});
      }, ev.enabled ? '' : 'on'));
      a1.appendChild(act(ev.done ? '\u64A4\u56DE' : '\u5B8C\u6210', function () {
        get('/sched/done?id=' + ev.id + '&on=' + (ev.done ? 0 : 1)).then(apply).catch(function () {});
      }, ev.done ? '' : 'hi'));
      c.appendChild(a1);

      // 时间快进：把测试时间挪到目标前 N 秒
      var a2 = document.createElement('div');
      a2.className = 'scacts';
      [[86400, 'T-1\u5929'], [3600, 'T-1\u65F6'], [600, 'T-10\u5206'], [60, 'T-1\u5206']]
        .forEach(function (p) {
          a2.appendChild(act(p[1], function () {
            get('/sched/time?e=' + (ev.target - p[0])).then(apply).catch(function () {});
          }, 'hi'));
        });
      c.appendChild(a2);

      box.appendChild(c);
    });
  }

  function startEdit(ev) {
    editingId = ev.id;
    el('scTitle').value = ev.title;
    for (var i = 0; i < typeSel.options.length; i++) {
      if (typeSel.options[i].value === ev.type) typeSel.selectedIndex = i;
    }
    var d = new Date(ev.target * 1000);
    el('scY').value  = d.getUTCFullYear();
    el('scMo').value = d.getUTCMonth() + 1;
    el('scD').value  = d.getUTCDate();
    el('scH').value  = d.getUTCHours();
    el('scMi').value = d.getUTCMinutes();
    el('scSave').textContent = '\u4FDD\u5B58\u4FEE\u6539';
    el('scCancel').style.display = '';
  }

  function stopEdit() {
    editingId = 0;
    el('scSave').textContent = '\u6DFB\u52A0';
    el('scCancel').style.display = 'none';
  }

  el('scCancel').onclick = function () {
    stopEdit();
    el('scTitle').value = '';
  };

  el('scSave').onclick = function () {
    var q = 'title=' + encodeURIComponent(el('scTitle').value) +
            '&type=' + typeSel.value +
            '&y=' + el('scY').value + '&mo=' + el('scMo').value +
            '&d=' + el('scD').value + '&h=' + (el('scH').value || 0) +
            '&mi=' + (el('scMi').value || 0);
    var url = editingId ? ('/sched/upd?id=' + editingId + '&' + q)
                        : ('/sched/add?' + q);
    get(url).then(function (r) {
      stopEdit();
      el('scTitle').value = '';
      apply(r);
    }).catch(function () {});
  };

  // ── 状态行更新 ──
  function apply(j) {
    if (!j) return;
    if (j.err && window.toast) { toast('\u65E5\u7A0B\uFF1A' + j.err); }

    function set(k, v, cls) {
      var n = el('sc_' + k);
      if (!n) return;
      n.textContent = v;
      n.className = 'v' + (cls ? ' ' + cls : '');
    }

    set('now',   j.synced ? fmtTarget(j.now) : '--', j.synced ? 'ok' : 'warn');
    set('sync',  j.synced ? (j.testTime ? '\u6D4B\u8BD5' : '\u6B63\u5E38') : '\u672A\u540C\u6B65',
                 j.synced ? (j.testTime ? 'warn' : 'ok') : 'warn');
    set('count', j.count + ' / ' + j.max, j.count >= j.max ? 'warn' : '');
    set('next',  j.nextTitle ? j.nextTitle : '--');
    set('saves', String(j.saveCount), j.saveCount > 0 ? '' : 'ok');
    set('store', j.storage ? 'NVS \u6B63\u5E38' : '\u5F02\u5E38', j.storage ? 'ok' : 'warn');

    renderList(j);
  }

  function refresh() {
    if (document.hidden) return;
    get('/sched/list').then(apply).catch(function () {});
  }

  // 默认把日期填成「现在 + 1 天」，省得每次手输
  (function seedDate() {
    var d = new Date(Date.now() + 86400000);
    el('scY').value  = d.getFullYear();
    el('scMo').value = d.getMonth() + 1;
    el('scD').value  = d.getDate();
    el('scH').value  = 9;
    el('scMi').value = 0;
  })();

  refresh();
  // 整合新增：面板收起时不轮询（展开后 <=1.5s 自动刷新）
  setInterval(function () { if (slot.offsetParent) refresh(); }, 1500);
})();
)rawjs";
