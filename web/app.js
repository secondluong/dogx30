// X30 遥控台。协议见 docs/app-protocol.md。
//
// 两条时间线：WebSocket 收遥测（10 Hz，被动），定时器发控制量（20 Hz，主动）。
// 发送必须独立于用户输入 —— 摇杆不动时也要持续发，否则网关看门狗会误判掉线。

'use strict';

const SEND_HZ = 20;
const HEARTBEAT_MS = 500;
const RECONNECT_MS = 1000;

const $ = (id) => document.getElementById(id);

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------

const app = {
  ws: null,
  clientId: 0,
  holder: 0,
  hasControl: false,
  alive: false,
  basicState: 0,
  gait: 'walk',
  gaitPending: false,
  left: { x: 0, y: 0 },   // 左摇杆：x=平移, y=前后
  right: { x: 0, y: 0 },  // 右摇杆：x=转向/偏航, y=俯仰
};

// 踏步态才走速度通道，力控站立走姿态通道。其余状态下摇杆无意义，直接禁用，
// 免得用户对着没反应的摇杆反复推。
const STATE_STEPPING = 4;
const STATE_TORQUE_STANDING = 3;

function controlChannel() {
  if (app.basicState === STATE_STEPPING) return 'vel';
  if (app.basicState === STATE_TORQUE_STANDING) return 'pose';
  return null;
}

// ---------------------------------------------------------------------------
// 连接
// ---------------------------------------------------------------------------

function connect() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const url = `${proto}//${location.host}/ws`;
  const ws = new WebSocket(url);
  app.ws = ws;

  ws.onopen = () => setLink(true);

  ws.onclose = () => {
    setLink(false);
    app.hasControl = false;
    app.holder = 0;
    renderControl();
    setTimeout(connect, RECONNECT_MS);
  };

  ws.onerror = () => ws.close();

  // 点云走二进制帧。不设这个的话浏览器给的是 Blob，还得走一次异步读取，
  // 2 Hz 的大帧那样会平白多一次拷贝和一次事件循环往返。
  ws.binaryType = 'arraybuffer';

  ws.onmessage = (ev) => {
    if (ev.data instanceof ArrayBuffer) {
      if (window.X30Cloud) window.X30Cloud.onCloudFrame(ev.data);
      return;
    }
    let msg;
    try { msg = JSON.parse(ev.data); } catch { return; }
    switch (msg.t) {
      case 'hello':
        app.clientId = msg.client_id;
        app.holder = msg.holder || 0;
        app.hasControl = !!msg.control;
        renderControl();
        break;
      case 'control':
        app.holder = msg.holder || 0;
        app.hasControl = app.holder === app.clientId;
        renderControl();
        break;
      case 'state':
        renderState(msg);
        break;
      case 'gait_result':
        onGaitResult(msg);
        break;
      case 'media_plan':
        if (window.X30Media) window.X30Media.onMediaPlan(msg, showBanner);
        break;
      case 'cloud_status':
        if (window.X30Cloud) window.X30Cloud.onCloudStatus(msg);
        break;
      case 'error':
        showBanner(msg.msg);
        break;
    }
  };
}

// 步态切换是异步的，楼梯尤其可能要几秒。切换期间把按钮禁掉，避免操作员
// 以为没反应而连点 —— 编排器会拒绝并发请求，连点只会刷出一堆报错。
function setGaitPending(pending) {
  app.gaitPending = pending;
  document.querySelectorAll('[data-gait]').forEach((b) => {
    b.disabled = pending;
  });
}

function onGaitResult(msg) {
  setGaitPending(false);
  if (msg.ok) {
    if (msg.msg) showBanner(msg.msg);
    return;
  }
  // 失败原因往往很长（要写清楚该去查什么），给足停留时间。
  showBanner(msg.msg || '步态切换失败', 9000);
}

function send(obj) {
  if (app.ws && app.ws.readyState === WebSocket.OPEN) {
    app.ws.send(JSON.stringify(obj));
  }
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

function setLink(online) {
  const chip = $('chip-link');
  chip.classList.toggle('online', online);
  $('link-text').textContent = online ? '已连接' : '重连中';
  if (!online) {
    document.querySelector('.telemetry').classList.add('stale');
  }
}

function renderControl() {
  const btn = $('btn-control');
  btn.classList.toggle('held', app.hasControl);
  btn.textContent = app.hasControl ? '释放控制权' : '申请控制权';

  const tag = $('tag-holder');
  tag.classList.toggle('held', app.hasControl);
  if (app.hasControl) {
    tag.textContent = '控制中';
  } else if (app.holder) {
    tag.textContent = `被 #${app.holder} 占用`;
  } else {
    tag.textContent = '观察者';
  }

  updateStickAvailability();
}

const fmt = (v, n = 2) => (typeof v === 'number' ? v.toFixed(n) : '—');

function renderState(s) {
  app.alive = !!s.alive;
  app.basicState = s.basic_state;

  document.querySelector('.telemetry').classList.toggle('stale', !s.alive);

  const chipState = $('chip-state');
  chipState.textContent = s.basic_state_text || '—';
  chipState.classList.toggle('online', s.alive);

  $('chip-gait').textContent = s.gait_text || '—';
  $('chip-batt').textContent = `${s.battery.level}% · ${fmt(s.battery.voltage, 1)}V`;

  $('t-vx').textContent = fmt(s.vel.x);
  $('t-vy').textContent = fmt(s.vel.y);
  $('t-wz').textContent = fmt(s.vel.yaw);
  $('t-ox').textContent = fmt(s.odom.x);
  $('t-oy').textContent = fmt(s.odom.y);
  $('t-yaw').textContent = fmt(s.att.yaw, 1);
  $('t-rp').textContent = `${fmt(s.att.roll, 1)} / ${fmt(s.att.pitch, 1)}`;
  $('t-batt').textContent = s.battery.level;
  $('t-cpu').textContent = fmt(s.temp.cpu, 1);
  $('t-motor').textContent = fmt(s.temp.motor_max, 1);
  $('t-mile').textContent = fmt(s.mileage_cm / 100, 1);
  $('t-limit').textContent = fmt(s.limits.forward, 2);

  const box = $('t-errors');
  box.innerHTML = '';
  (s.errors || []).forEach((e) => {
    const el = document.createElement('span');
    el.textContent = e;
    box.appendChild(el);
  });

  if (s.emergency_source) showBanner('机器人处于软急停状态，需在遥控器上解除');

  document.querySelectorAll('[data-gait]').forEach((b) => {
    b.classList.toggle('active', s.gait_key === b.dataset.gait);
  });
  document.querySelectorAll('[data-height]').forEach((b) => {
    b.classList.toggle('active',
      (s.height_gear === 0 && b.dataset.height === 'normal') ||
      (s.height_gear < 0 && b.dataset.height === 'crawl'));
  });

  updateStickAvailability();
}

let bannerTimer = null;
function showBanner(text, holdMs) {
  const el = $('banner');
  el.textContent = text;
  el.classList.remove('hidden');
  clearTimeout(bannerTimer);
  bannerTimer = setTimeout(() => el.classList.add('hidden'), holdMs || 4000);
}

function updateStickAvailability() {
  const usable = app.hasControl && app.alive && controlChannel() !== null;
  document.querySelectorAll('.stick').forEach((s) => {
    s.classList.toggle('disabled', !usable);
  });
}

// ---------------------------------------------------------------------------
// 虚拟摇杆
// ---------------------------------------------------------------------------

function makeStick(el, onChange) {
  const knob = el.querySelector('.knob');
  let pointerId = null;

  const radius = () => el.clientWidth / 2 - knob.clientWidth / 2;

  function move(clientX, clientY) {
    const rect = el.getBoundingClientRect();
    let dx = clientX - (rect.left + rect.width / 2);
    let dy = clientY - (rect.top + rect.height / 2);
    const r = radius();
    const dist = Math.hypot(dx, dy);
    if (dist > r) {
      dx = (dx / dist) * r;
      dy = (dy / dist) * r;
    }
    knob.style.transform = `translate(${dx}px, ${dy}px)`;
    // 屏幕坐标 y 向下为正，控制量前进为正，所以取反。
    onChange(dx / r, -dy / r);
  }

  function release() {
    pointerId = null;
    el.classList.remove('active');
    knob.style.transform = 'translate(0px, 0px)';
    onChange(0, 0);
  }

  el.addEventListener('pointerdown', (e) => {
    if (el.classList.contains('disabled')) return;
    pointerId = e.pointerId;
    el.setPointerCapture(pointerId);
    el.classList.add('active');
    move(e.clientX, e.clientY);
  });

  el.addEventListener('pointermove', (e) => {
    if (e.pointerId !== pointerId) return;
    move(e.clientX, e.clientY);
  });

  // pointercancel 同样要归零 —— 系统手势打断触摸时若不复位，摇杆会卡在最后位置。
  ['pointerup', 'pointercancel', 'lostpointercapture'].forEach((type) => {
    el.addEventListener(type, (e) => {
      if (e.pointerId !== pointerId) return;
      release();
    });
  });
}

makeStick($('stick-left'), (x, y) => { app.left.x = x; app.left.y = y; });
makeStick($('stick-right'), (x, y) => { app.right.x = x; app.right.y = y; });

// ---------------------------------------------------------------------------
// 发送回路
// ---------------------------------------------------------------------------

// 摇杆通道的统一约定见 gamepad.js 顶部。四个通道一律「推杆方向为正」：
// fwd 前进正、lat 左移正、turn 左转正、tilt 向上正。
//
// 之前触摸摇杆是直接往两种输出格式里塞值的，两处各写一套符号，
// 结果 vel 的 vy 和 pose 的 yaw 符号都反了 —— 同一个推杆动作在踏步态
// 和力控站立态下会往相反方向去。统一到中间层就没有这个出错的余地了。
function touchChannels() {
  return {
    fwd: app.left.y,
    lat: -app.left.x,     // 屏幕 x 向右为正，通道约定左为正
    turn: -app.right.x,
    tilt: app.right.y,
  };
}

function activeChannels() {
  // 物理手柄离开死区时优先于触摸摇杆。松开手柄就自动交还，
  // 不需要显式切换 —— 现场戴手套点屏幕本来就不方便。
  if (window.X30Gamepad) {
    const gp = window.X30Gamepad.channels();
    if (gp.engaged) return gp;
  }
  return touchChannels();
}

setInterval(() => {
  if (!app.hasControl) return;
  const channel = controlChannel();
  const c = activeChannels();
  if (channel === 'vel') {
    // 网关的速度接口收的是机体系：Y 左为正、偏航逆时针为正，与通道约定一致。
    send({ t: 'vel', vx: c.fwd, vy: c.lat, wz: c.turn });
  } else if (channel === 'pose') {
    // 姿态接口收的是原始轴语义：向右为正，与通道约定相反，故取负。
    send({ t: 'pose', h: c.fwd, roll: -c.lat, pitch: c.tilt, yaw: -c.turn });
  }
}, 1000 / SEND_HZ);

// 心跳独立于摇杆回路。狗起身、坐下的这几秒没有摇杆量可发，
// 光靠控制量喂租约会导致操作员在动作中途莫名丢失控制权。
let pingSeq = 0;
setInterval(() => send({ t: 'ping', id: ++pingSeq }), HEARTBEAT_MS);

// ---------------------------------------------------------------------------
// 按钮
// ---------------------------------------------------------------------------

$('btn-control').addEventListener('click', () => {
  send(app.hasControl ? { t: 'yield' } : { t: 'claim' });
});

// 急停不检查控制权，任何客户端任何时候都能按。
$('btn-estop').addEventListener('click', () => {
  send({ t: 'cmd', name: 'estop' });
  showBanner('已发送软急停');
});

function guarded(fn) {
  return () => {
    if (!app.hasControl) { showBanner('请先申请控制权'); return; }
    fn();
  };
}

document.querySelectorAll('[data-cmd]').forEach((b) => {
  b.addEventListener('click', guarded(() => send({ t: 'cmd', name: b.dataset.cmd })));
});
document.querySelectorAll('[data-gait]').forEach((b) => {
  b.addEventListener('click', guarded(() => {
    if (app.gaitPending) return;
    setGaitPending(true);
    send({
      t: 'cmd',
      name: 'gait',
      value: b.dataset.gait,
      stair_style: $('stair-style').value,
    });
    // 网关最长约 5 秒给结果（含等待静止）。兜底解禁，别把按钮永久锁死。
    setTimeout(() => { if (app.gaitPending) setGaitPending(false); }, 9000);
  }));
});
document.querySelectorAll('[data-height]').forEach((b) => {
  b.addEventListener('click', guarded(() => send({ t: 'cmd', name: 'height', value: b.dataset.height })));
});

// 切后台时立刻停车。平板锁屏或切 App 后定时器会被节流，
// 指令不再按时送达，机器人可能带着最后一次速度继续走。
document.addEventListener('visibilitychange', () => {
  if (document.hidden && app.hasControl) {
    send({ t: 'release' });
    showBanner('已切至后台，运动已停止');
  }
  // 后台时点云既看不见也渲染不了，继续收只是白占 MESH 带宽。
  if (document.hidden && window.X30Cloud) window.X30Cloud.stop();
});

connect();

if (window.X30Media) window.X30Media.initMedia(send, showBanner);
if (window.X30Cloud) window.X30Cloud.initCloud(send);
if (window.X30Gamepad) window.X30Gamepad.initGamepad(send, showBanner, () => app);
