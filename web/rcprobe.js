// 遥控器通道探测。G20/G30 的摇杆和大部分按键不进 Android Gamepad API，
// 只出现在云卓 RCSDK 的 PWM 数组里。换手柄之后通道会整表重排，
// 现场需要看见「按下去的是 CH 几、松开多少、按下多少」，才能改映射。
//
// 语法保守：不用可选链、不用 ??。

(function (root, factory) {
  var api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.X30RcProbe = api;
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  var MOVE_TH = 180;
  var LOG_MAX = 40;

  // App 里仍按 G20 现场表认功能。探测页对照着看，G30 对不上就按日志改。
  var G20_HINT = [
    { ch: 1, label: '右大摇杆左右（转向）' },
    { ch: 2, label: '右大摇杆上下（俯仰）' },
    { ch: 3, label: '左大摇杆前后（前进）' },
    { ch: 4, label: '左大摇杆左右（平移）' },
    { ch: 5, label: 'SW1 上=点云 / 中=狗身视频 / 下=双光' },
    { ch: 6, label: 'SW2 上=指标 / 中=开关 / 下=气体' },
    { ch: 7, label: 'L1 力控/起步' },
    { ch: 8, label: 'L2 起立/趴下/卸力' },
    { ch: 9, label: 'R1（未用）' },
    { ch: 10, label: 'HOME 对讲（开麦+听球 / 全关）' },
    { ch: 13, label: '左小左右：侦检=水平 / 水炮=炮台水平' },
    { ch: 14, label: '左小上下：侦检=俯仰 / 水炮=炮台俯仰' },
    { ch: 15, label: '右小左右：切画中画（右下一档 / 左上一档）' },
    { ch: 16, label: '右小前后：侦检=变焦 / 水炮=雾状·柱状' },
  ];

  function classify(rest, now) {
    if (typeof rest !== 'number' || typeof now !== 'number') return '';
    var d = now - rest;
    if (d !== d || Math.abs(d) < MOVE_TH) return '';
    if (Math.abs(now - 1050) < 150 || Math.abs(now - 1950) < 150) {
      return now < rest ? '按键（低）' : '按键（高）';
    }
    return '摇杆 / 拨轮 / 拨动';
  }

  function copyCh(ch) {
    var out = [];
    var i;
    for (i = 0; i < ch.length; i++) out.push(ch[i]);
    return out;
  }

  function makeProbe() {
    var rest = [];
    var armed = [];
    var log = [];
    var keys = [];
    var lastCh = [];
    var connected = false;
    var device = '';
    var error = '';

    function row(i) {
      var now = lastCh[i];
      var r = rest[i];
      var moved = typeof now === 'number' && typeof r === 'number' &&
                  Math.abs(now - r) >= MOVE_TH;
      return {
        ch: i + 1,
        rest: r,
        now: now,
        moved: moved,
        kind: moved ? classify(r, now) : '',
      };
    }

    function pushLog(line) {
      log.unshift(line);
      if (log.length > LOG_MAX) log.length = LOG_MAX;
    }

    function feedRc(ev) {
      ev = ev || {};
      connected = !!ev.connected;
      device = ev.device || '';
      error = ev.error || '';
      var ch = ev.ch;
      if (!ch || !ch.length) {
        lastCh = [];
        return { rows: [], events: [] };
      }
      if (!rest.length) rest = copyCh(ch);
      while (rest.length < ch.length) rest.push(ch[rest.length]);

      var events = [];
      var i;
      for (i = 0; i < ch.length; i++) {
        var v = ch[i];
        var r = rest[i];
        var moved = typeof v === 'number' && typeof r === 'number' &&
                    Math.abs(v - r) >= MOVE_TH;
        if (moved && !armed[i]) {
          var kind = classify(r, v);
          var evn = {
            ch: i + 1,
            rest: r,
            now: v,
            kind: kind,
          };
          events.push(evn);
          pushLog('CH' + evn.ch + '  松开 ' + r + ' → 现在 ' + v +
                  (kind ? '  ' + kind : ''));
        }
        armed[i] = moved;
      }
      lastCh = copyCh(ch);
      var rows = [];
      for (i = 0; i < ch.length; i++) rows.push(row(i));
      return { rows: rows, events: events };
    }

    function feedKey(ev) {
      if (!ev || ev.repeat) return null;
      if (!ev.down) return null;
      var name = ev.name || ('KEYCODE_' + ev.keyCode);
      var line = 'Android  ' + name + '  keyCode=' + ev.keyCode +
                 '  scan=' + ev.scanCode;
      keys.unshift(line);
      if (keys.length > LOG_MAX) keys.length = LOG_MAX;
      pushLog(line);
      return line;
    }

    function captureRest() {
      if (lastCh.length) rest = copyCh(lastCh);
      armed = [];
    }

    function clearLog() {
      log = [];
      keys = [];
    }

    function reset() {
      rest = [];
      armed = [];
      lastCh = [];
      log = [];
      keys = [];
      connected = false;
      device = '';
      error = '';
    }

    function report() {
      var lines = [];
      lines.push('遥控器通道探测');
      lines.push('设备 ' + (device || '（未知）'));
      lines.push('连接 ' + (connected ? '是' : '否') +
                 (error ? '  ' + error : ''));
      lines.push('');
      lines.push('当前通道（CH号从 1 起，数组下标 = CH-1）');
      var i;
      for (i = 0; i < lastCh.length; i++) {
        var r = rest[i];
        lines.push('CH' + (i + 1) + '  松开=' + r + '  现在=' + lastCh[i]);
      }
      lines.push('');
      lines.push('刚才按下 / 拨动');
      if (!log.length) lines.push('（还没有）');
      else for (i = 0; i < log.length; i++) lines.push(log[i]);
      return lines.join('\n');
    }

    return {
      feedRc: feedRc,
      feedKey: feedKey,
      captureRest: captureRest,
      clearLog: clearLog,
      reset: reset,
      report: report,
      rows: function () {
        var out = [];
        var i;
        for (i = 0; i < lastCh.length; i++) out.push(row(i));
        return out;
      },
      log: function () { return log.slice(); },
      keys: function () { return keys.slice(); },
      meta: function () {
        return { connected: connected, device: device, error: error };
      },
    };
  }

  var api = {
    MOVE_TH: MOVE_TH,
    G20_HINT: G20_HINT,
    classify: classify,
    makeProbe: makeProbe,
  };

  if (typeof document === 'undefined') return api;

  var probe = null;
  var open = false;
  var timer = 0;
  var lastSeq = -1;

  function $(id) { return document.getElementById(id); }

  function readRc() {
    if (!window.X30Native || !window.X30Native.pollRc) return null;
    try {
      var raw = window.X30Native.pollRc();
      if (!raw) return null;
      return typeof raw === 'string' ? JSON.parse(raw) : raw;
    } catch (e) {
      return null;
    }
  }

  function paint() {
    var st = $('rc-probe-stat');
    var grid = $('rc-probe-grid');
    var logEl = $('rc-probe-log');
    if (!probe || !st || !grid || !logEl) return;

    var meta = probe.meta();
    if (!window.X30Native || !window.X30Native.pollRc) {
      st.textContent = '这是网页控制台，读不到云卓通道。请在 App 里打开本页。';
    } else if (!meta.connected) {
      st.textContent = meta.error || '遥控器未连接。先关掉云卓助手和其他地面站。';
    } else {
      st.textContent = (meta.device || '云卓遥控器') +
                       ' · 已连接 · 一次只按一颗，对照「刚才按下」';
    }

    var rows = probe.rows();
    var html = '';
    var i;
    for (i = 0; i < rows.length; i++) {
      var r = rows[i];
      html += '<div class="rc-ch' + (r.moved ? ' on' : '') + '">' +
              '<b>CH' + r.ch + '</b>' +
              '<span>' + r.now + '</span>' +
              '</div>';
    }
    if (!rows.length) {
      html = '<div class="rc-ch"><span>还没有通道数据</span></div>';
    }
    grid.innerHTML = html;

    var lines = probe.log();
    logEl.textContent = lines.length ? lines.join('\n') : '按下一颗键或拨一下杆，这里会记下通道号和松开/按下值。';
  }

  function tick() {
    if (!open || !probe) return;
    var ev = readRc();
    if (ev && ev.seq !== lastSeq) {
      lastSeq = ev.seq;
      probe.feedRc(ev);
    } else if (ev) {
      probe.feedRc(ev);
    }
    paint();
  }

  function start() {
    open = true;
    lastSeq = -1;
    if (!probe) probe = makeProbe();
    else probe.reset();
    if (window.X30Gamepad && window.X30Gamepad.setMuted) {
      window.X30Gamepad.setMuted(true);
    }
    paint();
    if (timer) window.clearInterval(timer);
    timer = window.setInterval(tick, 100);
    tick();
  }

  function stop() {
    open = false;
    if (timer) {
      window.clearInterval(timer);
      timer = 0;
    }
    if (window.X30Gamepad && window.X30Gamepad.setMuted) {
      window.X30Gamepad.setMuted(false);
    }
  }

  function copyReport() {
    var text = probe ? probe.report() : '';
    var note = $('rc-probe-copy-note');
    function ok() {
      if (note) note.textContent = '已复制。发回来就能按通道改映射。';
    }
    function fail() {
      if (note) note.textContent = '复制失败，请长按下面日志全选。';
    }
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(ok).catch(fail);
      return;
    }
    try {
      var ta = document.createElement('textarea');
      ta.value = text;
      document.body.appendChild(ta);
      ta.select();
      document.execCommand('copy');
      document.body.removeChild(ta);
      ok();
    } catch (e) {
      fail();
    }
  }

  function initRcProbe() {
    var box = $('set-rcprobe-box');
    if (!box) return;
    var hint = $('rc-probe-hint');
    if (hint) {
      var bits = [];
      var i;
      for (i = 0; i < G20_HINT.length; i++) {
        bits.push('CH' + G20_HINT[i].ch + ' ' + G20_HINT[i].label);
      }
      hint.textContent = 'App 现在仍按 G20 认：' + bits.join('；') + '。';
    }
    var restBtn = $('rc-probe-rest');
    if (restBtn) {
      restBtn.addEventListener('click', function () {
        if (probe) probe.captureRest();
        paint();
      });
    }
    var clearBtn = $('rc-probe-clear');
    if (clearBtn) {
      clearBtn.addEventListener('click', function () {
        if (probe) probe.clearLog();
        paint();
      });
    }
    var copyBtn = $('rc-probe-copy');
    if (copyBtn) copyBtn.addEventListener('click', copyReport);
  }

  api.initRcProbe = initRcProbe;
  api.start = start;
  api.stop = stop;
  api.isOpen = function () { return open; };
  api.onNativeKey = function (ev) {
    if (!open || !probe) return;
    probe.feedKey(ev);
    paint();
  };

  return api;
});
