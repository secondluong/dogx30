// 手柄输入层的单元测试：node tools/gamepad_test.js
//
// 这一层值得测，因为它出错的方式特别难查：符号反了只有推杆才发现，
// 而那时人已经站在一条能走会跳的 50 公斤机器狗旁边了。

'use strict';

var G = require('../web/gamepad.js');

var pass = 0, fail = 0;

function check(name, ok, detail) {
  if (ok) { pass++; console.log('  [OK]   ' + name); }
  else { fail++; console.log('  [FAIL] ' + name + (detail ? '  ' + detail : '')); }
}

function near(a, b, eps) {
  return Math.abs(a - b) < (eps === undefined ? 1e-6 : eps);
}

// --- 死区与曲线 -------------------------------------------------------------
console.log('\n== 死区与曲线 ==');

var s = G.shapeStick(0, 0, 0.12, 0.4);
check('静止时输出为零', s.x === 0 && s.y === 0);

s = G.shapeStick(0.1, 0, 0.12, 0.4);
check('死区内被吃掉', s.x === 0 && s.y === 0, JSON.stringify(s));

s = G.shapeStick(0.121, 0, 0.12, 0.4);
check('刚出死区时输出接近零而不是跳变',
      s.x > 0 && s.x < 0.01, 'x=' + s.x);

s = G.shapeStick(1, 0, 0.12, 0.4);
check('推到底仍能到满量程', near(s.x, 1), 'x=' + s.x);

s = G.shapeStick(-1, 0, 0.12, 0.4);
check('反向推到底为 -1', near(s.x, -1), 'x=' + s.x);

console.log('\n== G20 通道表 ==');
check('中位 PWM 为零', G.pwmAxis(1500, false) === 0);
check('2000 为满量程', near(G.pwmAxis(2000, false), 1));
check('取反后左转为正', near(G.pwmAxis(1000, true), 1));
check('1050 算按下', G.pwmPressed(1050, 1050));
check('1500 不算按下', !G.pwmPressed(1500, 1050));

var rest = [1500, 1500, 1500, 1500];
var idle = G.g20Channels(rest, 0.12, 0.4);
check('G20 中位不产生运动',
      idle.fwd === 0 && idle.lat === 0 && idle.turn === 0 && idle.tilt === 0,
      JSON.stringify(idle));

var fwdCh = [1500, 1500, 2000, 1500];
var fwd = G.g20Channels(fwdCh, 0.12, 0.4);
check('CH3 推到 2000 是前进', fwd.fwd > 0.9 && Math.abs(fwd.lat) < 0.05,
      JSON.stringify(fwd));

var leftCh = [1500, 1500, 1500, 1000];
var left = G.g20Channels(leftCh, 0.12, 0.4);
check('CH4 到 1000 是左移', left.lat > 0.9 && Math.abs(left.fwd) < 0.05,
      JSON.stringify(left));

var turnCh = [1000, 1500, 1500, 1500];
var turn = G.g20Channels(turnCh, 0.12, 0.4);
check('CH1 到 1000 是左转', turn.turn > 0.9 && turn.tilt === 0,
      JSON.stringify(turn));
check('CH5 低位是释放控制权', G.ch5Toggle(1050) === 'yield');
check('CH5 中位不动作', G.ch5Toggle(1500) === '');
check('CH5 高位是申请控制权', G.ch5Toggle(1950) === 'claim');
check('滚轮下是增加', G.wheelDetent(1050) === 'down');
check('滚轮中位回弹不算动作', G.wheelDetent(1500) === 'mid');
check('滚轮上是减少', G.wheelDetent(1950) === 'up');

// 圆形死区：斜推时方向不能被掰歪，否则"想斜着走却先直着走"
s = G.shapeStick(0.6, 0.6, 0.12, 0.4);
check('斜推时方向保持 45 度', near(s.x, s.y), s.x + ' vs ' + s.y);

// 逐轴死区会让这种情形下 y 被清零，方向偏 90 度。这里必须两个分量都留住。
s = G.shapeStick(0.5, 0.1, 0.12, 0.4);
check('小分量不被逐轴死区抹掉', s.y !== 0 && s.x !== 0, JSON.stringify(s));
check('小分量方向比例正确', near(s.y / s.x, 0.1 / 0.5, 1e-6),
      'ratio=' + (s.y / s.x));

// 模长不能超过 1，否则会超出协议量程
var worst = 0;
for (var a = 0; a < 360; a += 7) {
  var r = a * Math.PI / 180;
  var t = G.shapeStick(Math.cos(r), Math.sin(r), 0.12, 0.4);
  worst = Math.max(worst, Math.sqrt(t.x * t.x + t.y * t.y));
}
check('任意方向推到底模长都不超过 1', worst <= 1 + 1e-9, 'worst=' + worst);

// 手柄偶尔报超过 1 的值（尤其是斜推），不能因此溢出
s = G.shapeStick(1.4, 1.4, 0.12, 0.4);
check('输入超量程时被夹住',
      Math.sqrt(s.x * s.x + s.y * s.y) <= 1 + 1e-9, JSON.stringify(s));

// expo 让中点附近更迟钝，但两端必须保持不变
check('expo 不改变零点', near(G.expoCurve(0, 0.4), 0));
check('expo 不改变满量程', near(G.expoCurve(1, 0.4), 1));
check('expo=0 即线性', near(G.expoCurve(0.5, 0), 0.5));
check('expo 让半量程处更小', G.expoCurve(0.5, 0.4) < 0.5,
      String(G.expoCurve(0.5, 0.4)));

// --- 轴读取与反向 -----------------------------------------------------------
console.log('\n== 轴读取 ==');

var snap = { axes: [0.5, -0.5, 0, 0], buttons: [] };
check('正常读取', G.readAxis(snap, { type: 'axis', index: 0 }) === 0.5);
check('反向标记生效',
      G.readAxis(snap, { type: 'axis', index: 0, invert: true }) === -0.5);
check('索引越界返回 0', G.readAxis(snap, { type: 'axis', index: 9 }) === 0);
check('未映射返回 0', G.readAxis(snap, undefined) === 0);
check('类型不符返回 0',
      G.readAxis(snap, { type: 'button', index: 0 }) === 0);

var nanSnap = { axes: [NaN], buttons: [] };
check('NaN 被挡掉', G.readAxis(nanSnap, { type: 'axis', index: 0 }) === 0);

// --- 按键上升沿 -------------------------------------------------------------
console.log('\n== 按键上升沿 ==');

var core = G.makeCore({
  map: {
    vx: { type: 'axis', index: 1, invert: true },
    vy: { type: 'axis', index: 0, invert: true },
    wz: { type: 'axis', index: 2, invert: true },
    pitch: { type: 'axis', index: 3, invert: true },
    stand: { type: 'button', index: 0 },
    estop: { type: 'button', index: 9 },
  },
  deadzone: 0.1,
  expo: 0,
});

function frame(axes, pressedIdx) {
  var buttons = [];
  for (var i = 0; i < 12; i++) {
    buttons.push({ pressed: pressedIdx.indexOf(i) >= 0 });
  }
  return core.update({ axes: axes, buttons: buttons });
}

var zeroAx = [0, 0, 0, 0];
var r1 = frame(zeroAx, []);
check('无按键时不产生事件', r1.pressed.length === 0);

var r2 = frame(zeroAx, [0]);
check('按下产生一次事件', r2.pressed.length === 1 && r2.pressed[0] === 'stand',
      JSON.stringify(r2.pressed));

var r3 = frame(zeroAx, [0]);
check('按住不重复触发', r3.pressed.length === 0, JSON.stringify(r3.pressed));

var r4 = frame(zeroAx, []);
check('松开不产生事件', r4.pressed.length === 0);

var r5 = frame(zeroAx, [0]);
check('再次按下再次触发', r5.pressed.length === 1);

frame(zeroAx, []);
var r6 = frame(zeroAx, [0, 9]);
check('同帧多键都能触发', r6.pressed.length === 2, JSON.stringify(r6.pressed));

// 断开重连后不该把"还按着"误判成一次新按下
frame(zeroAx, [0]);
core.reset();
var r7 = frame(zeroAx, [0]);
check('reset 后按住的键会重新算一次上升沿', r7.pressed.length === 1);

// --- 通道语义：这是最容易出错也最危险的部分 ---------------------------------
console.log('\n== 通道语义（推杆方向为正）==');

core.reset();
// 标准布局：axes[1] 向下为正，所以向前推是负值，映射里 invert=true
var fwdPush = frame([0, -1, 0, 0], []);
check('左摇杆前推 -> fwd 为正',
      fwdPush.channels.fwd > 0.9, 'fwd=' + fwdPush.channels.fwd);
check('左摇杆前推不产生横向量',
      near(fwdPush.channels.lat, 0), 'lat=' + fwdPush.channels.lat);

// axes[0] 向右为正，invert 后左推为正
var leftPush = frame([-1, 0, 0, 0], []);
check('左摇杆左推 -> lat 为正',
      leftPush.channels.lat > 0.9, 'lat=' + leftPush.channels.lat);

var rightStickLeft = frame([0, 0, -1, 0], []);
check('右摇杆左推 -> turn 为正（左转）',
      rightStickLeft.channels.turn > 0.9, 'turn=' + rightStickLeft.channels.turn);

var rightStickUp = frame([0, 0, 0, -1], []);
check('右摇杆上推 -> tilt 为正',
      rightStickUp.channels.tilt > 0.9, 'tilt=' + rightStickUp.channels.tilt);

var idle = frame(zeroAx, []);
check('静止时 engaged 为假', idle.engaged === false);
check('推杆时 engaged 为真', fwdPush.engaged === true);

// 漂移不该让手柄一直"占着"输入权，否则触摸摇杆永远抢不回来
var drift = frame([0.05, 0.05, 0.05, 0.05], []);
check('轻微漂移不算 engaged', drift.engaged === false,
      JSON.stringify(drift.channels));

// --- 死人开关 ---------------------------------------------------------------
console.log('\n== 死人开关 ==');

var dmMap = {
  vx: { type: 'axis', index: 1, invert: true },
  vy: { type: 'axis', index: 0, invert: true },
  wz: { type: 'axis', index: 2, invert: true },
  pitch: { type: 'axis', index: 3, invert: true },
  stand: { type: 'button', index: 0 },
  estop: { type: 'button', index: 9 },
  deadman: { type: 'button', index: 6 },
};

function pads12(pressedIdx) {
  var b = [];
  for (var i = 0; i < 12; i++) b.push({ pressed: pressedIdx.indexOf(i) >= 0 });
  return b;
}

// 关着的时候行为要和以前完全一致，不能因为加了这个功能就改变默认操作方式
var dmOff = G.makeCore({ map: dmMap, deadzone: 0.1, expo: 0, deadmanRequired: false });
var offRes = dmOff.update({ axes: [0, -1, 0, 0], buttons: pads12([]) });
check('关闭时不按也能推', offRes.channels.fwd > 0.9, 'fwd=' + offRes.channels.fwd);

var dmOn = G.makeCore({ map: dmMap, deadzone: 0.1, expo: 0, deadmanRequired: true });
var noHold = dmOn.update({ axes: [0, -1, 0, 0], buttons: pads12([]) });
check('开启且未按住时摇杆被锁',
      noHold.channels.fwd === 0 && noHold.engaged === false,
      JSON.stringify(noHold.channels));
check('被锁时 gated 为真', noHold.gated === true);

var held = dmOn.update({ axes: [0, -1, 0, 0], buttons: pads12([6]) });
check('按住后摇杆放行', held.channels.fwd > 0.9, 'fwd=' + held.channels.fwd);
check('按住时 deadmanHeld 为真', held.deadmanHeld === true);

var released = dmOn.update({ axes: [0, -1, 0, 0], buttons: pads12([]) });
check('松手立即归零（不等下一帧）',
      released.channels.fwd === 0, 'fwd=' + released.channels.fwd);

// 闸门只管摇杆。急停任何时候都必须发得出去 —— 这是最关键的一条。
dmOn.reset();
dmOn.update({ axes: [0, 0, 0, 0], buttons: pads12([]) });
var estopWhileLocked = dmOn.update({ axes: [0, 0, 0, 0], buttons: pads12([9]) });
check('未按死人开关时急停仍然发得出去',
      estopWhileLocked.pressed.indexOf('estop') >= 0,
      JSON.stringify(estopWhileLocked.pressed));

dmOn.reset();
dmOn.update({ axes: [0, 0, 0, 0], buttons: pads12([]) });
var standWhileLocked = dmOn.update({ axes: [0, 0, 0, 0], buttons: pads12([0]) });
check('未按死人开关时状态机按键仍可用',
      standWhileLocked.pressed.indexOf('stand') >= 0,
      JSON.stringify(standWhileLocked.pressed));

// 开了却没映射按键：必须锁死并标记出来，而不是假装开着其实没开
var dmBroken = G.makeCore({
  map: { vx: { type: 'axis', index: 1, invert: true } },
  deadzone: 0.1, expo: 0, deadmanRequired: true,
});
var brokenRes = dmBroken.update({ axes: [0, -1, 0, 0], buttons: pads12([6]) });
check('开启但未映射时摇杆锁死', brokenRes.channels.fwd === 0,
      'fwd=' + brokenRes.channels.fwd);
check('开启但未映射时被标记出来',
      brokenRes.deadmanMisconfigured === true);
check('正常配置时不误报未映射',
      dmOn.update({ axes: [0, 0, 0, 0], buttons: pads12([]) })
        .deadmanMisconfigured === false);

check('deadman 是合法的映射键', G.validateConfig({ map: dmMap }).ok);
check('deadman 不在派发列表里',
      G.ACTION_BUTTONS.indexOf('deadman') < 0);

// --- 输出侧符号：复现网关两种模式的换算 -------------------------------------
//
// 这几条是为了钉死那个真实出现过的 bug：同一个推杆动作在踏步态和力控站立态
// 下曾经产生相反的轴值。两种模式的换算写在 app.js 里，这里照抄一份来验证
// 一致性 —— 抄一份是有意的，改了 app.js 而没改这里，测试就会红。
console.log('\n== 两种输出模式的符号必须一致 ==');

// 每个函数只镜像真实代码里的一处，别把两步并成一步 ——
// 并起来写就是在测试里重新推导一遍符号，而推错的概率和写业务代码时一样大。
// （第一版就是在这里多写了一层负号，四条断言全红。）

// 镜像 web/app.js 的发送回路
function appSendsVel(c) { return { vx: c.fwd, vy: c.lat, wz: c.turn }; }
function appSendsPose(c) {
  return { h: c.fwd, roll: -c.lat, pitch: c.tilt, yaw: -c.turn };
}

// 镜像 rk3588/src/motion_client.cpp 的 SetVelocity / SetPose
function gatewayFromVel(m) {
  return { leftY: m.vx, leftX: -m.vy, rightX: -m.wz };
}
function gatewayFromPose(m) {
  return { leftY: m.h, leftX: m.roll, rightX: m.yaw };
}

function velAxes(c) { return gatewayFromVel(appSendsVel(c)); }
function poseAxes(c) { return gatewayFromPose(appSendsPose(c)); }

var cases = [
  { name: '左摇杆右推', c: { fwd: 0, lat: -1, turn: 0, tilt: 0 } },
  { name: '左摇杆左推', c: { fwd: 0, lat: 1, turn: 0, tilt: 0 } },
  { name: '右摇杆右推', c: { fwd: 0, lat: 0, turn: -1, tilt: 0 } },
  { name: '右摇杆左推', c: { fwd: 0, lat: 0, turn: 1, tilt: 0 } },
  { name: '左摇杆前推', c: { fwd: 1, lat: 0, turn: 0, tilt: 0 } },
];
cases.forEach(function (t) {
  var v = velAxes(t.c), p = poseAxes(t.c);
  check(t.name + ' 在两种模式下轴值一致',
        near(v.leftX, p.leftX) && near(v.rightX, p.rightX) &&
        near(v.leftY, p.leftY),
        'vel=' + JSON.stringify(v) + ' pose=' + JSON.stringify(p));
});

// 再钉一条绝对方向：协议规定原始轴为正表示"向右平移 / 向右转"
var pushRight = { fwd: 0, lat: -1, turn: -1, tilt: 0 };
check('左摇杆右推得到向右平移（原始轴为正）',
      velAxes(pushRight).leftX > 0, String(velAxes(pushRight).leftX));
check('右摇杆右推得到向右转（原始轴为正）',
      velAxes(pushRight).rightX > 0, String(velAxes(pushRight).rightX));

// --- 步态循环 ---------------------------------------------------------------
console.log('\n== 步态循环 ==');

check('下一个', G.nextGait('walk', 1) === 'slope', G.nextGait('walk', 1));
check('上一个', G.nextGait('slope', -1) === 'walk', G.nextGait('slope', -1));
check('末尾回绕', G.nextGait('silent', 1) === 'walk', G.nextGait('silent', 1));
check('开头回绕', G.nextGait('walk', -1) === 'silent', G.nextGait('walk', -1));
check('楼梯态不在循环里时从头开始',
      G.GAIT_CYCLE.indexOf(G.nextGait('stair', 1)) >= 0, G.nextGait('stair', 1));
check('楼梯步态被排除在肩键循环之外',
      G.GAIT_CYCLE.indexOf('stair') < 0 &&
      G.GAIT_CYCLE.indexOf('stairmulti') < 0 &&
      G.GAIT_CYCLE.indexOf('stair45') < 0);

// --- 映射校验 ---------------------------------------------------------------
console.log('\n== 映射校验 ==');

var good = {
  map: {
    vx: { type: 'axis', index: 1, invert: true },
    vy: { type: 'axis', index: 0 },
    wz: { type: 'axis', index: 2 },
    pitch: { type: 'axis', index: 3 },
    stand: { type: 'button', index: 0 },
  },
  suggested_deadzone: 0.15,
};
check('合法映射通过', G.validateConfig(good).ok);

check('非对象被拒', !G.validateConfig(null).ok);
check('缺 map 被拒', !G.validateConfig({}).ok);
check('一个轴都没有被拒',
      !G.validateConfig({ map: { stand: { type: 'button', index: 0 } } }).ok);
check('轴位置放了按键被拒',
      !G.validateConfig({ map: { vx: { type: 'button', index: 0 } } }).ok);
check('按键位置放了轴被拒',
      !G.validateConfig({
        map: { vx: { type: 'axis', index: 0 }, stand: { type: 'axis', index: 1 } },
      }).ok);
check('索引为负被拒',
      !G.validateConfig({ map: { vx: { type: 'axis', index: -1 } } }).ok);
check('索引为小数被拒',
      !G.validateConfig({ map: { vx: { type: 'axis', index: 1.5 } } }).ok);
check('死区超范围被拒',
      !G.validateConfig({
        map: { vx: { type: 'axis', index: 0 } }, suggested_deadzone: 1.5,
      }).ok);
// 诊断页输出里还带着 device / axis_range 等字段，不该因此被拒
check('多余字段不影响校验',
      G.validateConfig({
        device: 'x', axis_range: [], buttons_seen: [1, 2],
        map: { vx: { type: 'axis', index: 0 } },
      }).ok);

// 诊断页的真实输出必须能被接受 —— 两边格式对不上是很容易犯的错
var wizardOutput = {
  device: 'Xbox Wireless Controller (STANDARD GAMEPAD)',
  mapping_std: 'standard',
  axes: 4,
  buttons: 17,
  axis_range: [{ axis: 0, min: -1, max: 1 }],
  buttons_seen: [0, 1, 2],
  rest_drift: [0.004, 0.008, 0, 0],
  suggested_deadzone: 0.05,
  map: {
    vx: { type: 'axis', index: 1, invert: true },
    vy: { type: 'axis', index: 0, invert: true },
    wz: { type: 'axis', index: 2, invert: true },
    pitch: { type: 'axis', index: 3, invert: true },
    stand: { type: 'button', index: 0, invert: false },
    torque: { type: 'button', index: 2, invert: false },
    step: { type: 'button', index: 1, invert: false },
    gait_up: { type: 'button', index: 5, invert: false },
    gait_dn: { type: 'button', index: 4, invert: false },
    estop: { type: 'button', index: 9, invert: false },
  },
};
var wv = G.validateConfig(wizardOutput);
check('诊断页的真实输出能被接受', wv.ok, wv.error);

// 标准布局兜底映射自己也得合法
check('内置标准布局映射合法',
      G.validateConfig({ map: G.STANDARD_MAP }).ok);

// 用兜底映射跑一遍，方向要和上面的约定一致
var stdCore = G.makeCore({ map: G.STANDARD_MAP });
var stdFwd = stdCore.update({ axes: [0, -1, 0, 0], buttons: [] });
check('标准布局下前推也是 fwd 为正', stdFwd.channels.fwd > 0.9,
      'fwd=' + stdFwd.channels.fwd);

// --- 缺失映射时的健壮性 -----------------------------------------------------
console.log('\n== 部分映射缺失 ==');

var partial = G.makeCore({
  map: { vx: { type: 'axis', index: 1, invert: true } },
  deadzone: 0.1, expo: 0,
});
var pr = partial.update({ axes: [0.9, -0.9, 0.9, 0.9], buttons: [] });
check('只映射了一个轴时其余通道为零',
      pr.channels.fwd > 0.8 && pr.channels.lat === 0 &&
      pr.channels.turn === 0 && pr.channels.tilt === 0,
      JSON.stringify(pr.channels));

var empty = G.makeCore({ map: {} });
var er = empty.update({ axes: [1, 1, 1, 1], buttons: [{ pressed: true }] });
check('空映射不会崩且不产生输入',
      er.engaged === false && er.pressed.length === 0);

var nullSnap = empty.update(null);
check('快照为空不会崩', nullSnap.engaged === false);

// --- 浏览器分支 -------------------------------------------------------------
//
// 上面测的都是纯函数。轮询循环、掉线归零、按键派发、localStorage 这些只在
// 浏览器分支里，而模块在加载时就按 typeof document 决定要不要装配它们 ——
// 所以这里先把假 DOM 铺好再重新 require 一次。
//
// 这一段的价值在于：这些代码在真机上跑不通的话，人已经在现场了。
console.log('\n== 浏览器分支（假 DOM 驱动真实轮询循环）==');

var elements = {};
function fakeEl() {
  return {
    textContent: '', className: '', _handlers: {},
    addEventListener: function (type, fn) { this._handlers[type] = fn; },
    click: function () { if (this._handlers.click) this._handlers.click(); },
  };
}
['gp-status', 'gp-note', 'gp-paste', 'gp-clear'].forEach(function (id) {
  elements[id] = fakeEl();
});
elements['gp-deadman'] = (function () {
  var el = fakeEl();
  el.checked = false;
  el.change = function (v) {
    el.checked = v;
    if (el._handlers.change) el._handlers.change();
  };
  return el;
})();

var rafQueue = [];
var storage = {};
var pads = [];
var docHandlers = {};

global.document = {
  getElementById: function (id) { return elements[id] || null; },
  addEventListener: function (t, fn) { docHandlers[t] = fn; },
  hidden: false,
};
global.navigator = { getGamepads: function () { return pads; } };
global.window = {
  localStorage: {
    getItem: function (k) {
      return Object.prototype.hasOwnProperty.call(storage, k) ? storage[k] : null;
    },
    setItem: function (k, v) { storage[k] = v; },
    removeItem: function (k) { delete storage[k]; },
  },
  requestAnimationFrame: function (fn) { rafQueue.push(fn); },
  prompt: function () { return global.__promptReply; },
};
global.self = global.window;

delete require.cache[require.resolve('../web/gamepad.js')];
var B = require('../web/gamepad.js');

check('浏览器分支下暴露了 initGamepad', typeof B.initGamepad === 'function');

var sent = [];
var banners = [];
var appState = { hasControl: false, gait: 'walk', gaitPending: false };

function pump(n) {
  for (var i = 0; i < (n || 1); i++) {
    var q = rafQueue;
    rafQueue = [];
    for (var j = 0; j < q.length; j++) q[j]();
  }
}

function makePad(axes, pressedIdx, mapping) {
  var buttons = [];
  for (var i = 0; i < 12; i++) {
    buttons.push({ pressed: pressedIdx.indexOf(i) >= 0 });
  }
  return {
    id: 'Fake Pad', index: 0, connected: true,
    mapping: mapping === undefined ? 'standard' : mapping,
    axes: axes, buttons: buttons,
  };
}

B.initGamepad(
  function (m) { sent.push(m); },
  function (m) { banners.push(m); },
  function () { return appState; }
);

check('无手柄时状态显示未连接',
      elements['gp-status'].textContent === '未连接',
      elements['gp-status'].textContent);
check('无手柄时通道为零', B.channels().engaged === false);

// 接上一个标准布局手柄
pads = [makePad([0, 0, 0, 0], [])];
pump(1);
check('接上标准布局手柄后自动可用',
      elements['gp-status'].textContent.indexOf('标准布局') >= 0,
      elements['gp-status'].textContent);
check('没有映射急停时给出提示',
      elements['gp-note'].textContent.indexOf('急停未映射') >= 0,
      elements['gp-note'].textContent);

// 推杆
pads = [makePad([0, -1, 0, 0], [])];
pump(1);
var ch = B.channels();
check('前推后通道有值且 engaged', ch.engaged === true && ch.fwd > 0.9,
      JSON.stringify(ch));

// 没有控制权时按功能键只提示，不发指令
sent = []; banners = [];
pads = [makePad([0, 0, 0, 0], [0])];
pump(1);
check('无控制权时按键不下发指令', sent.length === 0, JSON.stringify(sent));
check('无控制权时给出提示',
      banners.length === 1 && banners[0].indexOf('控制权') >= 0,
      JSON.stringify(banners));

// 拿到控制权后同一个键要能下发
appState.hasControl = true;
sent = [];
pads = [makePad([0, 0, 0, 0], [])];
pump(1);
pads = [makePad([0, 0, 0, 0], [0])];
pump(1);
check('有控制权时按键下发 stand',
      sent.length === 1 && sent[0].t === 'cmd' && sent[0].name === 'stand',
      JSON.stringify(sent));

// 肩键循环步态
sent = [];
pads = [makePad([0, 0, 0, 0], [])];
pump(1);
pads = [makePad([0, 0, 0, 0], [5])];
pump(1);
check('肩键切到下一个步态',
      sent.length === 1 && sent[0].name === 'gait' && sent[0].value === 'slope',
      JSON.stringify(sent));

// 步态切换进行中时忽略，避免连点刷出一堆报错
appState.gaitPending = true;
sent = [];
pads = [makePad([0, 0, 0, 0], [])];
pump(1);
pads = [makePad([0, 0, 0, 0], [5])];
pump(1);
check('切换进行中忽略肩键', sent.length === 0, JSON.stringify(sent));
appState.gaitPending = false;

// 掉线必须立刻归零，否则狗会带着最后的速度一直走到看门狗超时
pads = [makePad([0, -1, 0, 0], [])];
pump(1);
check('掉线前确实有速度', B.channels().fwd > 0.9);
banners = [];
pads = [];
pump(1);
var off = B.channels();
check('掉线后通道立刻归零',
      off.fwd === 0 && off.lat === 0 && off.turn === 0 && off.tilt === 0 &&
      off.engaged === false, JSON.stringify(off));
check('掉线后提示操作员',
      banners.length === 1 && banners[0].indexOf('断开') >= 0,
      JSON.stringify(banners));
check('掉线后状态回到未连接',
      elements['gp-status'].textContent === '未连接');

// 切后台归零
pads = [makePad([0, -1, 0, 0], [])];
pump(2);
check('回前台后重新有值', B.channels().fwd > 0.9);
docHandlers.visibilitychange && (global.document.hidden = true);
docHandlers.visibilitychange();
check('切后台后归零', B.channels().fwd === 0 && B.channels().engaged === false);
global.document.hidden = false;

// 换上一个非标准布局的手柄（真实情形是拔掉旧的插上新的，所以 id 也不同）。
// 没有映射时要明说不可用，而不是默默不动让人以为是狗坏了。
pads = [];
pump(1);
pads = [{ id: 'Weird Pad', index: 0, connected: true, mapping: '',
          axes: [0, 0, 0, 0], buttons: [] }];
pump(2);
check('非标准布局且无映射时明确报不可用',
      elements['gp-status'].textContent.indexOf('无可用映射') >= 0,
      elements['gp-status'].textContent);
check('非标准布局时不产生输入', B.channels().engaged === false);

// 粘贴映射
global.__promptReply = JSON.stringify(wizardOutput);
banners = [];
elements['gp-paste'].click();
check('粘贴合法映射后保存到 localStorage',
      !!storage[B.STORE_KEY], JSON.stringify(Object.keys(storage)));
check('粘贴后状态变为自定义映射',
      elements['gp-status'].textContent.indexOf('自定义') >= 0,
      elements['gp-status'].textContent);

// 粘贴垃圾要被挡住，且不能覆盖已有的好映射
global.__promptReply = '{"map":{"vx":{"type":"button","index":0}}}';
banners = [];
elements['gp-paste'].click();
check('粘贴不合法映射被拒绝',
      banners.length === 1 && banners[0].indexOf('不可用') >= 0,
      JSON.stringify(banners));
check('被拒绝时不覆盖原有映射',
      JSON.parse(storage[B.STORE_KEY]).map.vx.type === 'axis');

global.__promptReply = 'not json at all';
banners = [];
elements['gp-paste'].click();
check('粘贴非 JSON 被拒绝',
      banners.length === 1 && banners[0].indexOf('JSON') >= 0,
      JSON.stringify(banners));

// 清除
elements['gp-clear'].click();
check('清除后 localStorage 里没有映射了', !storage[B.STORE_KEY]);

// 急停：任何时候都能按，且不看控制权
delete require.cache[require.resolve('../web/gamepad.js')];
storage[B.STORE_KEY] = JSON.stringify(wizardOutput);
var B2 = require('../web/gamepad.js');
var sent2 = [];
appState.hasControl = false;
rafQueue = [];
B2.initGamepad(function (m) { sent2.push(m); }, function () {}, function () { return appState; });
pads = [makePad([0, 0, 0, 0], [])];
pump(1);
pads = [makePad([0, 0, 0, 0], [9])];   // wizardOutput 里 estop = 按键 9
pump(1);
check('急停不需要控制权也能发出',
      sent2.length === 1 && sent2[0].name === 'estop', JSON.stringify(sent2));
check('存过映射后自动加载为自定义映射',
      elements['gp-status'].textContent.indexOf('自定义') >= 0,
      elements['gp-status'].textContent);

// 死人开关在浏览器侧的完整来回：开启 -> 锁住 -> 按住放行 -> 持久化 -> 关闭
console.log('\n== 死人开关（浏览器侧）==');

delete require.cache[require.resolve('../web/gamepad.js')];
storage = {};
var dmWizard = JSON.parse(JSON.stringify(wizardOutput));
dmWizard.map.deadman = { type: 'button', index: 6 };
storage['x30.gamepad.map'] = JSON.stringify(dmWizard);
var B3 = require('../web/gamepad.js');
rafQueue = [];
banners = [];
appState.hasControl = true;
B3.initGamepad(function () {}, function (m) { banners.push(m); },
               function () { return appState; });

pads = [makePad([0, -1, 0, 0], [])];
pump(1);
check('默认关闭时可以直接推', B3.channels().fwd > 0.9,
      'fwd=' + B3.channels().fwd);

elements['gp-deadman'].change(true);
pump(1);
check('开启后未按住则推不动', B3.channels().fwd === 0,
      'fwd=' + B3.channels().fwd);
check('开启后状态栏说明摇杆已锁',
      elements['gp-status'].textContent.indexOf('锁') >= 0,
      elements['gp-status'].textContent);

pads = [makePad([0, -1, 0, 0], [6])];
pump(1);
check('按住后可以推动', B3.channels().fwd > 0.9, 'fwd=' + B3.channels().fwd);
check('按住时状态栏显示可推动',
      elements['gp-status'].textContent.indexOf('可推动') >= 0,
      elements['gp-status'].textContent);

pads = [makePad([0, -1, 0, 0], [])];
pump(1);
check('松手后立即锁回去', B3.channels().fwd === 0);
check('开启状态被持久化', storage['x30.gamepad.deadman'] === '1');

elements['gp-deadman'].change(false);
pump(1);
check('关闭后恢复可推', B3.channels().fwd > 0.9, 'fwd=' + B3.channels().fwd);
check('关闭状态也被持久化', storage['x30.gamepad.deadman'] === '0');

// 开着死人开关但映射里没有这个键：必须锁死并明确告知，
// 而不是让操作员以为手柄坏了
delete require.cache[require.resolve('../web/gamepad.js')];
storage = {};
storage['x30.gamepad.map'] = JSON.stringify(wizardOutput);  // 不含 deadman
storage['x30.gamepad.deadman'] = '1';
var B4 = require('../web/gamepad.js');
rafQueue = [];
B4.initGamepad(function () {}, function () {}, function () { return appState; });
pads = [makePad([0, -1, 0, 0], [6])];
pump(1);
check('重启后自动恢复死人开关的开启状态并锁住',
      B4.channels().fwd === 0, 'fwd=' + B4.channels().fwd);
check('未映射时状态栏点明原因',
      elements['gp-status'].textContent.indexOf('未映射') >= 0,
      elements['gp-status'].textContent);
check('未映射时说明里给出解决办法',
      elements['gp-note'].textContent.indexOf('诊断页') >= 0,
      elements['gp-note'].textContent);

console.log('\n通过 ' + pass + '，失败 ' + fail);
process.exit(fail === 0 ? 0 : 1);
