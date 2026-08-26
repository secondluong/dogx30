// 物理手柄输入。映射由 gamepad.html 生成，本模块只负责读取与整形。
//
// 语法刻意保守（不用可选链、不用 ??）：工业手持地面站的 WebView 往往很旧，
// 而这个文件恰恰是只有在那台设备上才跑得起来的那个。
//
// ---------------------------------------------------------------------------
// 摇杆通道的统一约定
// ---------------------------------------------------------------------------
// 触摸摇杆、物理手柄两种输入，速度、姿态两种输出，两两组合就是四套符号约定。
// 各自为政的结果是符号错得悄无声息 —— 现场表现为"平移方向反了"，
// 而且只在某一种状态下反，很难查。
//
// 所以中间统一到四个通道，**推杆方向为正**：
//
//   fwd    左摇杆上下   前进为正
//   lat    左摇杆左右   左移为正
//   turn   右摇杆左右   左转为正
//   tilt   右摇杆上下   向上为正
//
// 输入侧负责转换到这套约定，输出侧负责从这套约定转出去。
// 新增输入源（比如厂家 SDK 的摇杆）只要产出这四个值即可。

(function (root, factory) {
  var api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.X30Gamepad = api;
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  var STORE_KEY = 'x30.gamepad.map';

  // 死人开关的开关另存一个键：它是操作策略，不该被"重新跑一遍映射向导"冲掉。
  var DEADMAN_KEY = 'x30.gamepad.deadman';

  // 摇杆通道 -> gamepad.html 里的目标名。历史原因那边用的是速度语义的名字，
  // 但它们其实就是四个物理通道，在姿态模式下含义不同。
  var CHANNEL_TARGET = {
    fwd: 'vx',
    lat: 'vy',
    turn: 'wz',
    tilt: 'pitch',
  };

  // 成对的通道属于同一根摇杆，死区要按半径算而不是按轴各算。
  // 按轴各算会切出一个十字形的死区，斜推时先被吃掉一个分量，
  // 手感上就是"斜着推会先直着走一下"。
  var PAIRS = [['lat', 'fwd'], ['turn', 'tilt']];

  // 会派发成指令的按键。
  var ACTION_BUTTONS = ['stand', 'torque', 'step', 'gait_up', 'gait_dn', 'estop'];

  // 映射里合法的按键，比上面多一个死人开关 —— 它不派发指令，只做闸门。
  var BUTTON_TARGETS = ACTION_BUTTONS.concat(['deadman']);

  // 手柄肩键循环步态时只在这些之间转。三种楼梯步态被有意排除：
  // 它们要和感知主机的地形图配合、切换要几秒，误触一下代价太大。
  // 楼梯仍然从屏幕上切。
  var GAIT_CYCLE = ['walk', 'slope', 'offroad', 'lwalk', 'mountain', 'silent'];

  var DEFAULT_TUNING = { deadzone: 0.12, expo: 0.4 };

  // 标准布局手柄（Xbox 手柄以及绝大多数 USB 手柄）的兜底映射，
  // 省得在办公桌上拿普通手柄联调时还要先跑一遍向导。
  // 标准布局里摇杆向下、向右为正，而本模块约定向上、向左为正，所以全都反向。
  //
  // 急停**故意不给默认值**：在一个不认识的手柄上瞎猜急停键，
  // 按错的代价比没有更大。屏幕上的急停按钮始终可用。
  var STANDARD_MAP = {
    vx: { type: 'axis', index: 1, invert: true },
    vy: { type: 'axis', index: 0, invert: true },
    wz: { type: 'axis', index: 2, invert: true },
    pitch: { type: 'axis', index: 3, invert: true },
    stand: { type: 'button', index: 0 },
    step: { type: 'button', index: 1 },
    torque: { type: 'button', index: 2 },
    gait_dn: { type: 'button', index: 4 },
    gait_up: { type: 'button', index: 5 },
  };

  // W3C standard mapping。Xbox / 多数 USB 手柄走这套；非标准布局只显示 bN。
  var STANDARD_BTN = [
    'A / 南', 'B / 东', 'X / 西', 'Y / 北',
    'LB', 'RB', 'LT', 'RT',
    'Back', 'Start', 'L3', 'R3',
    '↑', '↓', '←', '→', 'Home',
  ];
  var STANDARD_AXIS = ['左摇杆X', '左摇杆Y', '右摇杆X', '右摇杆Y'];
  var FUNC_LABEL = {
    stand: '坐/站', step: '起步', torque: '力控',
    gait_up: '步态上', gait_dn: '步态下',
    estop: '急停', deadman: '死人开关',
    vx: '前进/后退', vy: '左右平移', wz: '转向', pitch: '俯仰',
  };

  // --- 纯函数部分（可在 node 里直接测） -----------------------------------

  function clamp(v, lo, hi) {
    return v < lo ? lo : (v > hi ? hi : v);
  }

  // 三次曲线。中心附近变化慢，便于在狭窄空间里微调；末端仍能到满量程。
  function expoCurve(v, e) {
    return e * v * v * v + (1 - e) * v;
  }

  // 圆形死区 + 曲线整形。方向保持不变，只改变模长 ——
  // 逐分量做曲线会把斜推的方向掰歪。
  function shapeStick(x, y, deadzone, expo) {
    var m = Math.sqrt(x * x + y * y);
    if (m <= deadzone || m === 0) return { x: 0, y: 0 };
    var norm = clamp((m - deadzone) / (1 - deadzone), 0, 1);
    var shaped = expoCurve(norm, expo);
    return { x: (x / m) * shaped, y: (y / m) * shaped };
  }

  function readAxis(snapshot, m) {
    if (!m || m.type !== 'axis') return 0;
    var v = snapshot.axes[m.index];
    if (typeof v !== 'number' || v !== v) return 0;   // NaN 也挡掉
    v = clamp(v, -1, 1);
    return m.invert ? -v : v;
  }

  function isPressed(snapshot, m) {
    if (!m || m.type !== 'button') return false;
    var b = snapshot.buttons[m.index];
    if (b === undefined || b === null) return false;
    if (typeof b === 'object') return !!b.pressed;
    return b > 0.5;    // 有些实现直接给数值
  }

  // 把一帧手柄快照整形成四个通道值 + 本帧按下的按键（上升沿）。
  function makeCore(config) {
    var cfg = config || {};
    var map = cfg.map || {};
    var tuning = {
      deadzone: typeof cfg.deadzone === 'number' ? cfg.deadzone : DEFAULT_TUNING.deadzone,
      expo: typeof cfg.expo === 'number' ? cfg.expo : DEFAULT_TUNING.expo,
    };
    var prev = {};
    var deadmanRequired = !!cfg.deadmanRequired;
    var hasDeadmanKey = !!(cfg.map && cfg.map.deadman);

    function update(snapshot) {
      var channels = { fwd: 0, lat: 0, turn: 0, tilt: 0 };

      // 死人开关只闸摇杆，不闸按键。急停必须任何时候都发得出去；
      // 坐站、起步这些是有意识的单次按键，本来就不是"松手就该停"的东西。
      var deadmanHeld = snapshot ? isPressed(snapshot, map.deadman) : false;
      // 开了死人开关却没映射按键时，摇杆一律不放行。这看着别扭，
      // 但另一条路是"以为开着其实没开"，那才真危险。状态栏会说清楚原因。
      var gated = deadmanRequired && !deadmanHeld;

      if (snapshot && !gated) {
        for (var p = 0; p < PAIRS.length; p++) {
          var xKey = PAIRS[p][0], yKey = PAIRS[p][1];
          var rawX = readAxis(snapshot, map[CHANNEL_TARGET[xKey]]);
          var rawY = readAxis(snapshot, map[CHANNEL_TARGET[yKey]]);
          var s = shapeStick(rawX, rawY, tuning.deadzone, tuning.expo);
          channels[xKey] = s.x;
          channels[yKey] = s.y;
        }
      }

      var pressed = [];
      for (var i = 0; i < ACTION_BUTTONS.length; i++) {
        var key = ACTION_BUTTONS[i];
        var now = snapshot ? isPressed(snapshot, map[key]) : false;
        // 只认上升沿。按住不放不该被当成连续触发 ——
        // 60 Hz 轮询下按一下会变成刷屏式的几十条指令。
        if (now && !prev[key]) pressed.push(key);
        prev[key] = now;
      }

      var engaged = channels.fwd !== 0 || channels.lat !== 0 ||
                    channels.turn !== 0 || channels.tilt !== 0;

      return {
        channels: channels, pressed: pressed, engaged: engaged,
        deadmanHeld: deadmanHeld,
        // 开着死人开关却没映射按键 —— 手柄推不动，UI 要能解释清楚
        deadmanMisconfigured: deadmanRequired && !hasDeadmanKey,
        gated: gated,
      };
    }

    function reset() {
      prev = {};
    }

    return { update: update, reset: reset, tuning: tuning, map: map };
  }

  // 现场量过的 G20 通道。数组下标 = CH号 - 1。中位 1500，按键按下约 1050，
  // 但 B1/B2（CH9、CH10）反过来，按下是 1950 —— 所以判定要按中位分两边。
  // 右摇杆 CH1/CH2 只用来转向：CH2 参与死区，不进俯仰，避免上下推变成抬头。
  // PWM 增大按「右 / 前」理解；通道约定左移、左转为正，所以 lat/turn 取反。
  function pwmAxis(v, invert) {
    if (typeof v !== 'number' || v !== v) return 0;
    var n = clamp((v - 1500) / 500, -1, 1);
    return invert ? -n : n;
  }

  function pwmPressed(v, press) {
    if (typeof v !== 'number' || v !== v) return false;
    var mid = (press + 1500) / 2;
    return press < 1500 ? v <= mid : v >= mid;
  }

  function g20Channels(ch, deadzone, expo) {
    var dz = typeof deadzone === 'number' ? deadzone : DEFAULT_TUNING.deadzone;
    var ex = typeof expo === 'number' ? expo : DEFAULT_TUNING.expo;
    var left = shapeStick(pwmAxis(ch[3], true), pwmAxis(ch[2], false), dz, ex);
    var right = shapeStick(pwmAxis(ch[0], true), pwmAxis(ch[1], false), dz, ex);
    return {
      fwd: left.y,
      lat: left.x,
      turn: right.x,
      tilt: 0,
      look: right.y,
    };
  }

  var G20_BTN = {
    stand_up: { ch: 10, press: 1050 },
    sit_down: { ch: 6, press: 1050 },
    // B1 / B2（现场量的是 CH9、CH10，下标 = CH号-1）。这两颗按下是 1950 而不是
    // 1050，pwmPressed 按中位分方向，两种都认。
    torque: { ch: 8, press: 1950 },
    step: { ch: 9, press: 1950 },
    shot: { ch: 7, press: 1050 },
    talk: { ch: 5, press: 1050 },
    view_next: { ch: 11, press: 1050 },
    estop: { ch: 12, press: 1050 },
    telem: { ch: 14, press: 1050 },
    gas: { ch: 15, press: 1050 },
  };

  // 三段拨动：高位向上控狗，低位向下控布控球。中位保持上一档，不切模式。
  function ch5Toggle(v) {
    if (typeof v !== 'number' || v !== v) return '';
    if (v <= 1100) return 'ptz';
    if (v >= 1900) return 'dog';
    return '';
  }

  function wheelDetent(v) {
    if (typeof v !== 'number' || v !== v) return 'mid';
    if (v <= 1300) return 'down';
    if (v >= 1700) return 'up';
    return 'mid';
  }

  function nextGait(current, delta) {
    var i = GAIT_CYCLE.indexOf(current);
    // 当前步态不在循环里（多半正处于楼梯态）时，从头开始而不是原地不动。
    if (i < 0) return GAIT_CYCLE[delta > 0 ? 0 : GAIT_CYCLE.length - 1];
    var n = GAIT_CYCLE.length;
    return GAIT_CYCLE[((i + delta) % n + n) % n];
  }

  // 校验从诊断页粘回来的映射。宁可在这里报错，也不要拿一个半截的映射上狗。
  function validateConfig(obj) {
    if (!obj || typeof obj !== 'object') return { ok: false, error: '不是合法的 JSON 对象' };
    var map = obj.map;
    if (!map || typeof map !== 'object') return { ok: false, error: '缺少 map 字段' };

    var known = {};
    var k;
    for (k in CHANNEL_TARGET) known[CHANNEL_TARGET[k]] = 'axis';
    for (var i = 0; i < BUTTON_TARGETS.length; i++) known[BUTTON_TARGETS[i]] = 'button';

    var axes = 0;
    for (k in map) {
      if (!Object.prototype.hasOwnProperty.call(map, k)) continue;
      if (!known[k]) continue;                 // 多余的键忽略，不算错
      var m = map[k];
      if (!m || typeof m !== 'object') return { ok: false, error: k + ' 的值不是对象' };
      if (m.type !== 'axis' && m.type !== 'button') {
        return { ok: false, error: k + ' 的 type 只能是 axis 或 button' };
      }
      if (typeof m.index !== 'number' || m.index < 0 || m.index !== Math.floor(m.index)) {
        return { ok: false, error: k + ' 的 index 不是非负整数' };
      }
      if (known[k] !== m.type) {
        return { ok: false, error: k + ' 应该是 ' + known[k] + '，映射里却是 ' + m.type };
      }
      if (m.type === 'axis') axes++;
    }
    if (axes === 0) return { ok: false, error: '一个摇杆轴都没映射，这样手柄推不动狗' };

    var dz = obj.suggested_deadzone;
    if (dz !== undefined && dz !== null &&
        (typeof dz !== 'number' || dz < 0 || dz >= 1)) {
      return { ok: false, error: 'suggested_deadzone 要在 0 和 1 之间' };
    }
    return { ok: true };
  }

  var api = {
    STORE_KEY: STORE_KEY,
    DEADMAN_KEY: DEADMAN_KEY,
    CHANNEL_TARGET: CHANNEL_TARGET,
    ACTION_BUTTONS: ACTION_BUTTONS,
    BUTTON_TARGETS: BUTTON_TARGETS,
    GAIT_CYCLE: GAIT_CYCLE,
    STANDARD_MAP: STANDARD_MAP,
    DEFAULT_TUNING: DEFAULT_TUNING,
    expoCurve: expoCurve,
    shapeStick: shapeStick,
    readAxis: readAxis,
    isPressed: isPressed,
    makeCore: makeCore,
    nextGait: nextGait,
    validateConfig: validateConfig,
    STANDARD_BTN: STANDARD_BTN,
    STANDARD_AXIS: STANDARD_AXIS,
    FUNC_LABEL: FUNC_LABEL,
    onNativeKey: function (ev) {
      if (api._onNativeKey) api._onNativeKey(ev);
    },
    onNativeAxis: function (ev) {
      if (api._onNativeAxis) api._onNativeAxis(ev);
    },
    onRcChannels: function (ev) {
      if (api._onRcChannels) api._onRcChannels(ev);
    },
    pwmAxis: pwmAxis,
    pwmPressed: pwmPressed,
    g20Channels: g20Channels,
    ch5Toggle: ch5Toggle,
    wheelDetent: wheelDetent,
  };

  // --- 浏览器侧 -------------------------------------------------------------

  if (typeof document === 'undefined') return api;

  // 实体键上没有字，按的时候人也不在看屏幕 —— 这条路上语音是唯一的回执，
  // 所以每一条派发出去的指令都念一声，被拦下的也念（见 app.radioHint）。
  function say(text) {
    if (window.X30Voice) window.X30Voice.say(text);
  }

  // 步态的中文名只在屏幕按钮上写着一份，肩键循环时照着念，不再抄一份对照表。
  function gaitName(key) {
    var el = document.querySelector('[data-gait="' + key + '"]');
    if (!el) return key;
    return el.getAttribute('data-say') || el.textContent.trim();
  }

  var state = {
    core: null,
    source: 'none',        // none | standard | custom
    padId: '',
    padMapping: null,
    connected: false,
    channels: { fwd: 0, lat: 0, turn: 0, tilt: 0 },
    engaged: false,
    hasEstop: false,
    hasDeadman: false,
    deadmanOn: false,
    deadmanHeld: false,
    warned: false,
    g20Seq: -1,
    g20Prev: {},
    g20Talk: false,
    g20Wheel: null,
    g20Primed: false,
    stickTarget: 'dog',
  };

  function loadStored() {
    try {
      var raw = window.localStorage.getItem(STORE_KEY);
      if (!raw) return null;
      var obj = JSON.parse(raw);
      var v = validateConfig(obj);
      if (!v.ok) return null;
      return obj;
    } catch (e) {
      return null;
    }
  }

  function buildCore(stored, pad) {
    if (stored) {
      state.source = 'custom';
      state.hasEstop = !!stored.map.estop;
      state.hasDeadman = !!stored.map.deadman;
      return makeCore({
        map: stored.map,
        deadzone: stored.suggested_deadzone,
        expo: stored.expo,
        deadmanRequired: state.deadmanOn,
      });
    }
    if (pad && pad.mapping === 'standard') {
      state.source = 'standard';
      state.hasEstop = false;
      state.hasDeadman = false;
      return makeCore({ map: STANDARD_MAP, deadmanRequired: state.deadmanOn });
    }
    state.source = 'none';
    state.hasEstop = false;
    state.hasDeadman = false;
    return null;
  }

  function firstPad() {
    var pads = navigator.getGamepads ? navigator.getGamepads() : [];
    for (var i = 0; i < pads.length; i++) {
      if (pads[i] && pads[i].connected) return pads[i];
    }
    return null;
  }

  function zero() {
    state.channels = { fwd: 0, lat: 0, turn: 0, tilt: 0, look: 0 };
    state.engaged = false;
  }

  function initGamepad(send, showBanner, getApp) {
    var stored = loadStored();
    var statusEl = document.getElementById('gp-status');
    var noteEl = document.getElementById('gp-note');
    var deadmanEl = document.getElementById('gp-deadman');

    try {
      state.deadmanOn = window.localStorage.getItem(DEADMAN_KEY) === '1';
    } catch (e) {
      state.deadmanOn = false;
    }
    if (deadmanEl) deadmanEl.checked = state.deadmanOn;

    function setStatus() {
      if (!statusEl) return;
      if (!state.connected) {
        statusEl.textContent = '未连接';
        statusEl.className = 'tag';
      } else if (state.deadmanOn && !state.hasDeadman) {
        statusEl.textContent = '死人开关未映射，摇杆已锁';
        statusEl.className = 'tag tag-warn';
      } else if (state.deadmanOn) {
        statusEl.textContent = state.deadmanHeld ? '已握持，可推动' : '松开中，摇杆已锁';
        statusEl.className = state.deadmanHeld ? 'tag tag-ok' : 'tag';
      } else if (state.source === 'custom') {
        statusEl.textContent = '已连接（自定义映射）';
        statusEl.className = 'tag tag-ok';
      } else if (state.source === 'g20') {
        statusEl.textContent = '已连接（云卓 G20）';
        statusEl.className = 'tag tag-ok';
      } else if (state.source === 'standard') {
        statusEl.textContent = '已连接（标准布局）';
        statusEl.className = 'tag tag-ok';
      } else {
        statusEl.textContent = '已连接，但无可用映射';
        statusEl.className = 'tag tag-warn';
      }
      if (noteEl) {
        if (!state.connected) {
          noteEl.textContent = '插上手柄后拨动摇杆即可识别。没有映射时请先打开诊断页。';
        } else if (state.source === 'none') {
          noteEl.textContent = state.padId + ' —— 非标准布局，需要在诊断页里生成映射。';
        } else if (state.deadmanOn && !state.hasDeadman) {
          // 这条要排在急停提示前面：它意味着手柄此刻完全推不动，
          // 不说清楚的话现场会以为手柄坏了。
          noteEl.textContent = '死人开关已开启，但没有映射对应按键，' +
            '手柄暂时推不动狗。请到诊断页设定该键，或关掉这个选项。';
        } else if (!state.hasEstop) {
          noteEl.textContent = state.padId + ' —— 急停未映射，请用屏幕上的急停按钮。';
        } else {
          noteEl.textContent = state.padId;
        }
      }
    }

    function dispatch(key) {
      var app = getApp();
      function radioPose(cmd) {
        if (!(app.radioOnly && app.radioOnly())) return false;
        if (!app.nativeRadioCmd) return false;
        app.nativeRadioCmd(cmd);
        if (app.applyRadioPose) app.applyRadioPose(cmd);
        return true;
      }

      if (key === 'estop') {
        if (!radioPose('estop')) send({ t: 'cmd', name: 'estop' });
        showBanner('已发送软急停（手柄）');
        say('急停');
        return;
      }
      if (key === 'shot') {
        if (window.X30Capture && window.X30Capture.shotCurrent) {
          window.X30Capture.shotCurrent(showBanner);
        }
        return;
      }
      if (key === 'rec') {
        if (window.X30Capture && window.X30Capture.toggleRecCurrent) {
          window.X30Capture.toggleRecCurrent(showBanner);
        }
        return;
      }
      if (key === 'telem') {
        if (app.toggleTelem) app.toggleTelem();
        return;
      }
      if (key === 'gas') {
        if (app.toggleGas) app.toggleGas();
        return;
      }
      if (key === 'view_next') {
        if (app.cycleView) app.cycleView();
        return;
      }
      if (key === 'dog' || key === 'ptz') return;
      if (key === 'claim') {
        if (app.radioOnly ? app.radioOnly() : (!app.hasControl && app.radioFallback)) {
          if (app.radioHint) app.radioHint('g20');
          else showBanner('网关未连，请用 G20 摇杆走已对频的 2.4G，不必点控制权');
          return;
        }
        // 走 app.claimMsg：刚从 2.4G 切过来时这条 claim 要把姿态一并带给网关，
        // 自己拼一个空 claim 会把交接吞掉（表现是切回 MESH 又显示「起立」）。
        if (!app.hasControl) {
          send(app.claimMsg ? app.claimMsg() : { t: 'claim' });
          say('申请控制权');
        }
        return;
      }
      if (key === 'yield') {
        if (app.hasControl) {
          send({ t: 'yield' });
          say('已交还控制权');
        }
        return;
      }
      if (app.radioOnly && app.radioOnly()) {
        if (key === 'stand_up' || key === 'sit_down' || key === 'stand') {
          if (app.emergencyLocked) {
            if (radioPose('unload')) {
              say('卸力');
              return;
            }
          } else if (key === 'stand_up' && app.isStandingUi && app.isStandingUi()) {
            // 什么都没发出去。不念的话人听不出是「按到了但没必要」还是「没按上」。
            say('已经站着');
            return;
          } else if (key === 'sit_down' && app.isStandingUi && !app.isStandingUi()) {
            say('已经趴着');
            return;
          } else {
            var want = key === 'stand'
              ? (app.isStandingUi && app.isStandingUi() ? 'sit_down' : 'stand_up')
              : key;
            if (radioPose(want)) {
              say(want === 'sit_down' ? '趴下' : '起立');
              return;
            }
          }
        } else if (key === 'torque' || key === 'step') {
          if (radioPose(key)) {
            say(key === 'torque' ? '力控' : '起步');
            return;
          }
        } else if (key === 'gait_up' || key === 'gait_dn') {
          if (app.gaitPending) {
            say('步态切换中');
            return;
          }
          var next = nextGait(app.gait, key === 'gait_up' ? 1 : -1);
          if (radioPose(next)) {
            say(gaitName(next));
            return;
          }
        }
        if (app.radioHint) app.radioHint('g20');
        else showBanner('当前 2.4G：按键走无线电，不经网关');
        return;
      }
      if (!app.hasControl) {
        if (app.radioFallback) {
          if (app.radioHint) app.radioHint('g20');
          else showBanner('当前 2.4G：按键走无线电，不经网关');
          return;
        }
        showBanner('请先申请控制权');
        say('没有控制权');
        return;
      }
      if (key === 'stand_up' || key === 'sit_down' || key === 'stand') {
        // 急停自锁时原厂 ⑤/㉑ 是卸力，不是 RL 起/趴。
        if (app.emergencyLocked) {
          send({ t: 'cmd', name: 'unload' });
          say('卸力');
          return;
        }
        if (key === 'stand_up' && app.isStandingUi && app.isStandingUi()) {
          say('已经站着');
          return;
        }
        if (key === 'sit_down' && app.isStandingUi && !app.isStandingUi()) {
          say('已经趴着');
          return;
        }
        send({ t: 'cmd', name: 'stand' });
        say(app.isStandingUi && app.isStandingUi() ? '趴下' : '起立');
        return;
      }
      if (key === 'torque' || key === 'step') {
        if (key === 'step' && app.lioAligning) {
          showBanner('LIO 还在对准，请站稳，不要走', 4000);
          say('LIO 对准中');
          return;
        }
        send({ t: 'cmd', name: key });
        say(key === 'torque' ? '力控' : '起步');
        return;
      }
      if (key === 'gait_up' || key === 'gait_dn') {
        if (app.gaitPending) {
          say('步态切换中');
          return;
        }
        var target = nextGait(app.gait, key === 'gait_up' ? 1 : -1);
        // 和高亮规则一样：先按切的显示，网关回结果再确认或退回。
        if (app.markGait) app.markGait(target);
        send({ t: 'cmd', name: 'gait', value: target });
        say(gaitName(target));
      }
    }

    function setTalk(on) {
      if (state.g20Talk === on) return;
      state.g20Talk = on;
      if (window.X30Media && window.X30Media.setTalk) {
        window.X30Media.setTalk(on, showBanner);
      }
    }

    function applyG20(ev) {
      if (!ev || !ev.ch || !ev.ch.length) return false;
      if (ev.connected === false) return false;
      state.connected = true;
      state.source = 'g20';
      state.padId = ev.device || 'G20';
      var ch = g20Channels(ev.ch);
      state.channels = ch;
      state.engaged = ch.fwd !== 0 || ch.lat !== 0 || ch.turn !== 0;
      var name;
      var primed = state.g20Primed;
      for (name in G20_BTN) {
        if (!Object.prototype.hasOwnProperty.call(G20_BTN, name)) continue;
        var spec = G20_BTN[name];
        var down = ev.ch.length > spec.ch && pwmPressed(ev.ch[spec.ch], spec.press);
        // 首帧只记档：上电时通道常是 0/1050，会把 R1/R2 当成按下，指标和气体就被打开。
        if (primed) {
          if (name === 'talk') {
            setTalk(down);
          } else if (down && !state.g20Prev[name]) {
            dispatch(name);
          }
        }
        state.g20Prev[name] = down;
      }
      state.g20Primed = true;
      if (ev.ch.length > 4) {
        var tog = ch5Toggle(ev.ch[4]);
        if (state.g20Prev.toggle === undefined) {
          state.g20Prev.toggle = tog;
          if (tog === 'ptz' || tog === 'dog') state.stickTarget = tog;
        } else if (tog && tog !== state.g20Prev.toggle) {
          state.g20Prev.toggle = tog;
          if (tog === 'ptz' || tog === 'dog') {
            if (state.stickTarget !== tog) {
              state.stickTarget = tog;
              showBanner(tog === 'ptz' ? '摇杆：布控球' : '摇杆：机器狗');
              say(tog === 'ptz' ? '摇杆控布控球' : '摇杆控机器狗');
            }
          }
        } else if (!tog) {
          state.g20Prev.toggle = '';
        }
      }
      if (ev.ch.length > 13) {
        var detent = wheelDetent(ev.ch[13]);
        if (state.g20Wheel === null) {
          state.g20Wheel = detent;
        } else if (detent !== state.g20Wheel) {
          if (state.g20Wheel === 'mid' && window.X30Cloud && window.X30Cloud.nudgeZoom) {
            if (detent === 'down') window.X30Cloud.nudgeZoom(1);
            if (detent === 'up') window.X30Cloud.nudgeZoom(-1);
          }
          state.g20Wheel = detent;
        }
      }
      return true;
    }

    function readNativeG20() {
      if (!window.X30Native || !window.X30Native.pollRc) return null;
      try {
        var raw = window.X30Native.pollRc();
        if (!raw) return null;
        return typeof raw === 'string' ? JSON.parse(raw) : raw;
      } catch (e) {
        return null;
      }
    }

    api._onRcChannels = function (ev) {
      applyG20(ev);
    };

    function poll() {
      var g20 = readNativeG20();
      if (g20 && g20.connected && g20.ch && g20.ch.length) {
        applyG20(g20);
        window.requestAnimationFrame(poll);
        return;
      }
      if (state.source === 'g20') {
        if (!g20 || g20.connected === false) {
          state.source = 'none';
          state.connected = false;
          state.g20Primed = false;
          state.g20Prev = {};
          state.g20Wheel = null;
          state.stickTarget = 'dog';
          zero();
          setTalk(false);
        }
        window.requestAnimationFrame(poll);
        return;
      }

      var pad = firstPad();
      if (!pad) {
        if (state.connected) {
          state.connected = false;
          state.source = 'none';
          zero();
          if (state.core) state.core.reset();
          setTalk(false);
          setStatus();
          showBanner('手柄已断开，运动量已归零');
        }
        window.requestAnimationFrame(poll);
        return;
      }

      if (!state.connected || state.padId !== pad.id ||
          state.padMapping !== pad.mapping) {
        state.connected = true;
        state.padId = pad.id;
        state.padMapping = pad.mapping;
        state.core = buildCore(stored, pad);
        setStatus();
        if (state.source === 'none' && !state.warned) {
          state.warned = true;
          showBanner('手柄已连接但布局不认识，请打开诊断页生成映射', 8000);
        }
      }

      if (!state.core) {
        zero();
        window.requestAnimationFrame(poll);
        return;
      }

      var out = state.core.update({ axes: pad.axes, buttons: pad.buttons });
      state.channels = out.channels;
      state.engaged = out.engaged;
      if (state.deadmanHeld !== out.deadmanHeld) {
        state.deadmanHeld = out.deadmanHeld;
        setStatus();
      }
      for (var i = 0; i < out.pressed.length; i++) dispatch(out.pressed[i]);
      window.requestAnimationFrame(poll);
    }

    document.addEventListener('visibilitychange', function () {
      if (document.hidden) {
        zero();
        setTalk(false);
        if (state.core) state.core.reset();
      }
    });

    var pasteBtn = document.getElementById('gp-paste');
    if (pasteBtn) {
      pasteBtn.addEventListener('click', function () {
        var text = window.prompt('粘贴诊断页生成的 JSON：');
        if (!text) return;
        var obj;
        try {
          obj = JSON.parse(text);
        } catch (e) {
          showBanner('不是合法的 JSON');
          return;
        }
        var v = validateConfig(obj);
        if (!v.ok) {
          showBanner('映射不可用：' + v.error, 8000);
          return;
        }
        window.localStorage.setItem(STORE_KEY, JSON.stringify(obj));
        stored = obj;
        state.core = buildCore(stored, firstPad());
        setStatus();
        showBanner('映射已保存');
      });
    }

    if (deadmanEl) {
      deadmanEl.addEventListener('change', function () {
        state.deadmanOn = !!deadmanEl.checked;
        try {
          window.localStorage.setItem(DEADMAN_KEY, state.deadmanOn ? '1' : '0');
        } catch (e) { /* 隐私模式下存不了 */ }
        state.core = buildCore(stored, firstPad());
        setStatus();
        if (state.deadmanOn && !state.hasDeadman) {
          showBanner('死人开关已开启，但还没映射按键 —— 手柄现在推不动，请先去诊断页设定', 9000);
        } else {
          showBanner(state.deadmanOn ? '死人开关已开启：按住才能推动' : '死人开关已关闭');
        }
      });
    }

    var clearBtn = document.getElementById('gp-clear');
    if (clearBtn) {
      clearBtn.addEventListener('click', function () {
        window.localStorage.removeItem(STORE_KEY);
        stored = null;
        state.core = buildCore(null, firstPad());
        setStatus();
        showBanner('已清除映射');
      });
    }

    setStatus();
    poll();
  }

  api.initGamepad = initGamepad;
  api.stickTarget = function () { return state.stickTarget; };
  api.channels = function () {
    return { fwd: state.channels.fwd, lat: state.channels.lat,
             turn: state.channels.turn, tilt: state.channels.tilt,
             look: state.channels.look || 0,
             engaged: state.engaged, source: state.source,
             stickTarget: state.stickTarget };
  };
  return api;
});
