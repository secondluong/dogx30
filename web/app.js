// X30 遥控台。协议见 docs/app-protocol.md。
//
// 两条时间线：WebSocket 收遥测（10 Hz，被动），定时器发控制量（20 Hz，主动）。
// 发送必须独立于用户输入 —— 摇杆不动时也要持续发，否则网关看门狗会误判掉线。

'use strict';

const SEND_HZ = 20;
const HEARTBEAT_MS = 500;
const RECONNECT_MS = 1000;

const $ = (id) => document.getElementById(id);

// 安卓壳用 ?shell=app 打开。网页不加这个参数，布局完全不动。
// 没 MESH 时走 file:///android_asset，WebView 常把 ?shell=app 丢掉，
// 只认查询串会画出网页那套 2×2 / 控制权 / 摇杆。
function detectAppShell() {
  if (document.documentElement.classList.contains('shell-app')) return true;
  try {
    if (new URLSearchParams(location.search).get('shell') === 'app') return true;
  } catch (e) { /* 无 URLSearchParams 时看地址 */ }
  const href = String(location.href || '');
  return location.protocol === 'file:' || href.indexOf('android_asset') !== -1;
}
const isAppShell = detectAppShell();
if (isAppShell) document.documentElement.classList.add('shell-app');

const RADIO_STORE = 'x30.radioPath';

// 改一次网页就把这个字符串往前挪一位。界面上印出来，就能一眼看出
// assets/web 是不是真的重拷过 —— 编包漏拷是这套壳最常见的「改了没反应」。
const WEB_BUILD = '0825q';

function nativeAppVersion() {
  try {
    return String(window.X30Native.getAppVersion() || '');
  } catch (e) {
    return '';
  }
}

// 版本印在设置面板里，不占顶栏 —— 顶栏是遥控时要盯着的地方。
// 但不能不印：装包时漏拷 assets/web，页面就是旧的，而这一点光看界面看不出来。
function paintVerChip() {
  const el = $('set-app-ver');
  if (!el) return;
  const apk = nativeAppVersion();
  el.textContent = apk ? ('包 ' + apk + ' · 网页 ' + WEB_BUILD) : ('网页 ' + WEB_BUILD);
}

function nativeRadioPath() {
  try {
    const v = window.X30Native.getRadioPath();
    if (v === 'radio' || v === 'mesh') return v;
  } catch (e) { /* 网页没有原生桥 */ }
  return '';
}

function loadRadioPath() {
  if (!isAppShell) return 'mesh';
  const fromNative = nativeRadioPath();
  if (fromNative) return fromNative;
  try {
    const v = window.localStorage.getItem(RADIO_STORE);
    if (v === 'radio' || v === 'mesh') return v;
  } catch (e) { /* 无 storage 时按上次按钮 */ }
  return 'mesh';
}

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------

const app = {
  ws: null,
  clientId: 0,
  holder: 0,
  hasControl: false,
  radioPath: loadRadioPath(),
  radioFallback: false,   // radioPath==='radio' 的别名，手柄分支沿用
  alive: false,
  basicState: 0,
  rlStanding: false,
  emergencyLocked: false,
  controlMode: 0,
  gait: 'walk',
  gaitPending: false,
  lioAligning: false,
  walkMode: null,         // 由遥测推断：'torque' | 'step'
  torqueByUser: false,    // 人点的力控（要用摇杆调姿态），不是自动踩台阶踩上来的
  stairPick: 'stair',     // 上下楼用哪种楼梯步态。必选一项，所以有默认值
  poseHandoff: null,      // 待交接给网关的姿态（切档时记下，网关认了才算完）
  poseHintWarned: false,  // 旧网关的提示只说一次，别在遥控时反复弹
  modePick: null,         // G20 三挡或点按：manual | assist | auto
  left: { x: 0, y: 0 },   // 左摇杆：x=平移, y=前后
  right: { x: 0, y: 0 },  // 右摇杆：x=转向/偏航, y=俯仰
};
app.radioFallback = app.radioPath === 'radio';
window.app = app;

// 定时器在本脚本抛错时依然会触发，所以先把启动排进队列，再往下挂按钮。
let booted = false;
setTimeout(bootstrap, 0);

// 踏步态才走速度通道，力控站立走姿态通道。其余状态下摇杆无意义，直接禁用，
// 免得用户对着没反应的摇杆反复推。
const STATE_STEPPING = 4;
const STATE_TORQUE_STANDING = 3;
const STATE_SIT_TO_STAND = 1;
const STATE_STAND_TO_SIT = 5;
const STATE_INITIAL_STAND = 2;
const STATE_EMERGENCY = 6;

// 趴下只露起立；站立（含 RL 起立后遥测仍报 0）才出步态/身高和力控起步。
function isStandingUi() {
  if (app.rlStanding && app.basicState !== STATE_STAND_TO_SIT) return true;
  const s = app.basicState;
  return s === STATE_INITIAL_STAND || s === STATE_TORQUE_STANDING ||
         s === STATE_STEPPING || s === STATE_STAND_TO_SIT;
}
app.isStandingUi = isStandingUi;

function controlChannel() {
  // 操作员自己点了「力控」，摇杆就是调姿态（身高/横滚/俯仰/偏航）—— 这是力控站立
  // 唯一的用处，得留着。没点过就一律走速度通道：推杆就是要走，力控/踏步那两级
  // 台阶由网关（ArmForWalk）和 2.4G 那侧（RadioLink.armForWalk）自己踩掉。
  // 以前这里一进力控站立就改发姿态，操作员推杆只在原地起伏，只能再点一次起步。
  if (app.torqueByUser &&
      (app.basicState === STATE_TORQUE_STANDING || app.walkMode === 'torque')) {
    return 'pose';
  }
  if (app.basicState === STATE_STEPPING) return 'vel';
  if (app.basicState === STATE_TORQUE_STANDING) return 'vel';
  // RL 起立后遥测仍报 0，力控/起步常被主机忽略。原厂此时走速度通道。
  if (app.rlStanding &&
      app.basicState !== STATE_SIT_TO_STAND &&
      app.basicState !== STATE_STAND_TO_SIT) {
    return 'vel';
  }
  if (app.walkMode === 'step') return 'vel';
  return null;
}

function effectiveWalk() {
  if (app.basicState === STATE_STEPPING) return 'step';
  if (app.basicState === STATE_TORQUE_STANDING) return 'torque';
  return app.walkMode;
}

function paintWalkButtons() {
  const walk = effectiveWalk();
  document.querySelectorAll('[data-cmd="torque"]').forEach((b) => {
    b.classList.toggle('on', walk === 'torque');
  });
  document.querySelectorAll('[data-cmd="step"]').forEach((b) => {
    b.classList.toggle('on', walk === 'step');
    b.disabled = !!app.lioAligning;
  });
}

// ---------------------------------------------------------------------------
// 连接
// ---------------------------------------------------------------------------

function meshWsUrl() {
  try {
    if (isAppShell) {
      const host = String(window.X30Native.getGatewayHost() || '').trim();
      const port = Number(window.X30Native.getGatewayPort()) || 8080;
      if (host) return 'ws://' + host + ':' + port + '/ws';
    }
  } catch (e) { /* 用当前页地址 */ }
  if (location.protocol === 'file:') return '';
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  return proto + '//' + location.host + '/ws';
}

// 2.4G 只接管运动指令（见 send 里的过滤），网关连接照旧要建：机身相机和布控球的
// 拉流地址由网关下发（media_plan），遥测、电量、点云也都走它。曾经在 2.4G 下直接
// 不连网关，结果控制正常但整块画面是空的 —— 视频和操控本来就是两条独立的链路。
function connect() {
  syncNativeRadioPath();
  if (app.ws && (app.ws.readyState === WebSocket.OPEN
      || app.ws.readyState === WebSocket.CONNECTING)) {
    return;
  }
  const url = meshWsUrl();
  if (!url) {
    setLink(false);
    renderControl();
    setTimeout(connect, RECONNECT_MS);
    return;
  }
  const ws = new WebSocket(url);
  app.ws = ws;

  ws.onopen = () => {
    setLink(true);
    app.wsWasOpen = true;
    // 重连后网关不记得订阅。本机还标着已订的话要重新发一次，
    // 不然 2×2 左上角会停在「未订阅」，画布上却还留着上一帧。
    if (window.X30Cloud && window.X30Cloud.resubscribe) {
      window.X30Cloud.resubscribe();
    }
    if (window.X30Media && window.X30Media.onLinkOpen) {
      window.X30Media.onLinkOpen();
    }
  };

  ws.onclose = () => {
    const dropFromLive = !!app.wsWasOpen;
    app.wsWasOpen = false;
    if (dropFromLive && !isAppShell) app.radioFallback = true;
    app.hasControl = false;
    app.holder = 0;
    setLink(false);
    renderControl();
    if (dropFromLive) {
      showBanner(isAppShell
        ? (app.radioPath === 'radio'
          ? '网关已断，当前仍是 2.4G 直达'
          : 'MESH 已断。要控狗请切 2.4G，或等网关重连')
        : 'WiFi 已断，网关已放手。请用原厂 2.4G 手柄直达（无画面）', 8000);
    }
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
    try { msg = JSON.parse(ev.data); } catch (e) { return; }
    switch (msg.t) {
      case 'hello':
        app.clientId = msg.client_id;
        app.holder = msg.holder || 0;
        app.hasControl = !!msg.control;
        renderControl();
        if (window.X30Settings) window.X30Settings.onHello(msg);
        if (isAppShell && app.radioPath === 'mesh') requestControl();
        else if (isAppShell && app.radioPath === 'radio' && app.hasControl) {
          send({ t: 'yield' });
        }
        break;
      case 'config':
        if (window.X30Settings) window.X30Settings.onConfig(msg);
        break;
      case 'config_saved':
        if (window.X30Settings) window.X30Settings.onConfigSaved(msg);
        break;
      case 'control':
        app.holder = msg.holder || 0;
        app.hasControl = app.holder === app.clientId;
        renderControl();
        if (msg.granted === false) {
          showBanner(app.holder && app.holder !== app.clientId
            ? '控制权被 #' + app.holder + ' 占用'
            : '申请控制权失败');
        }
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
        // 配置相关的报错归设置面板显示在表单旁边，横幅四秒就没了，
        // 而人这时正盯着表单等结果。
        if (window.X30Settings && window.X30Settings.onError(msg)) break;
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
  paintPickers();
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

function hasNativeRadio() {
  // 安卓 WebView 里 X30Native 的方法 typeof 常常不是 function，
  // 用 typeof === 'function' 会误判成没有 2.4G，点了也不开数传。
  try {
    return !!window.X30Native;
  } catch (e) {
    return false;
  }
}

function radioDirect() {
  if (!isAppShell) return false;
  if (app.radioPath === 'radio') return true;
  return nativeRadioPath() === 'radio';
}

function send(obj) {
  if (!obj) return;
  if (obj.t === 'cmd') noteWalkIntent(obj.name);
  // 只有安装包真能直达时，2.4G 才停掉网关运动。否则切 2.4G 等于把 MESH 也掐死。
  if (radioDirect() && hasNativeRadio()) {
    const t = obj.t;
    if (t === 'claim' || t === 'vel' || t === 'pose' || t === 'ptz') return;
    if (t === 'cmd' && obj.name !== 'estop') return;
  }
  // 走网关发过起立/趴下之后，网关那份记忆就是它自己刚写的，比我们从 2.4G 带过来
  // 的旧认知新。不在这里放手，操作员在 MESH 下按趴下之后界面还会显示站立。
  if (obj.t === 'cmd' && POSE_CMDS[obj.name]) app.poseHandoff = null;
  if (app.ws && app.ws.readyState === WebSocket.OPEN) {
    app.ws.send(JSON.stringify(obj));
  }
}

// 会改变姿态的那几条。与 RadioLink.POSE_CMDS 同一份清单。
const POSE_CMDS = {
  stand: true, stand_up: true, sit: true, sit_down: true,
  unload: true, estop: true,
};

// 点了这些之后，摇杆重新回到「推杆就是要走」。与 RadioLink 里清掉 torqueByUser 的
// 时机一致：起步是明确要走，起立/趴下/卸力/急停把状态机打回去，力控意图不该留着。
const WALK_INTENT_CMDS = ['step', 'stand', 'stand_up', 'sit', 'sit_down',
                          'unload', 'estop'];

// 推杆是要走还是要调姿态，只由这一处记。两条链路的指令都会流过 send 或
// applyRadioPose，所以放那两处调，不必在每个按钮上各记一遍。
function noteWalkIntent(name) {
  if (name === 'torque') app.torqueByUser = true;
  else if (WALK_INTENT_CMDS.indexOf(name) >= 0) app.torqueByUser = false;
}

function linkOpen() {
  return !!(app.ws && app.ws.readyState === WebSocket.OPEN);
}
app.linkOpen = linkOpen;

function radioOnly() {
  if (radioDirect()) return true;
  return !linkOpen() || (app.radioFallback && !app.hasControl);
}
app.radioOnly = radioOnly;

function radioHint(kind) {
  if (radioDirect()) {
    showBanner(kind === 'g20'
      ? '当前 2.4G：起立/趴下/行走直达运动主机'
      : '2.4G 姿态没发出去。请先切回 MESH 看画面，或重装 App');
    return;
  }
  showBanner(kind === 'g20'
    ? '网关未连。要备份请切到 2.4G'
    : '网关未连，屏幕按钮走不了。请先恢复 MESH');
}
app.radioHint = radioHint;

function nativeRadioCmd(name) {
  try {
    window.X30Native.radioCmd(String(name));
    return true;
  } catch (e) {
    return false;
  }
}
app.nativeRadioCmd = nativeRadioCmd;

function hasRadioStanding() {
  return hasNativeRadio();
}

function nativeRadioLinkOk() {
  try {
    return !!window.X30Native.radioLinkOk();
  } catch (e) {
    return false;
  }
}

function nativeRadioStatus() {
  try {
    return JSON.parse(window.X30Native.radioStatus() || '{}');
  } catch (e) {
    return null;
  }
}

function nativeRadioStanding() {
  try {
    return !!window.X30Native.radioStanding();
  } catch (e) {
    return app.rlStanding;
  }
}


// 2.4G 下狗的姿态来自它自己的 UDP 遥测（RadioLink 解 0x1009），写进的是
// MESH 那侧同一个 app.basicState。两条链路读同一份真相，切档时状态才对得上 ——
// 以前 2.4G 只按「我点过什么」记，切回 MESH 前的旧读数还留在 app 里，
// 于是切一次就总显示站立。
function syncRadioStanding(st0) {
  if (!radioDirect()) return;
  const st = st0 || nativeRadioStatus() || {};
  const basic = typeof st.basic === 'number' ? st.basic : -1;
  let changed = false;
  if (basic >= 0 && basic !== app.basicState) {
    app.basicState = basic;
    if (basic === STATE_STEPPING) app.walkMode = 'step';
    else if (basic === STATE_TORQUE_STANDING) app.walkMode = 'torque';
    else app.walkMode = null;   // 其余状态都不在力控/踏步里，别留着旧的亮着
    changed = true;
  }
  if (basic < 0 && app.basicState !== 0) {
    // 收不到遥测（狗的 network.toml 里没登记这台平板）时，不能拿 MESH 那会儿的
    // 旧读数当真 —— 那正是「切一次档就总显示站立」的来路。退回本地记的那份。
    app.basicState = 0;
    changed = true;
  }
  if (basic >= 0) {
    const locked = basic === STATE_EMERGENCY;
    if (locked !== app.emergencyLocked) {
      app.emergencyLocked = locked;
      changed = true;
    }
  }
  // statusJson 里已经带了姿态，别再多走一次桥：每次桥调用都是一趟同步 Binder，
  // 而这个回路按发轴频率在跑。
  const up = typeof st.standing === 'boolean' ? st.standing : nativeRadioStanding();
  if (up !== app.rlStanding) {
    app.rlStanding = up;
    if (!up) app.walkMode = null;
    changed = true;
  }
  if (!changed) return;
  const wrap = $('stage-wrap');
  if (wrap) {
    const standing = isStandingUi();
    wrap.classList.toggle('dog-up', standing);
    wrap.classList.toggle('dog-prone', !standing);
  }
  paintStandButton();
  paintWalkButtons();
  updateStickAvailability();
}

// 2.4G 下狗自己报的步态/身高。有它就不必靠「我这次点过什么」——
// 刚开机或刚从 MESH 切过来时我们什么都没点过，菜单上却该显出真实档位。
const RADIO_GAIT_KEYS = {
  0: 'walk', 1: 'offroad', 2: 'slope', 3: 'run', 6: 'stair',
  7: 'stairmulti', 8: 'stair45', 32: 'lwalk', 33: 'mountain', 34: 'silent',
};

let radioGaitSeen = '';
let radioHeightSeen = null;

function syncRadioPickers(st) {
  // 这个回路按发轴频率在跑，读数没变就别重扫一遍 DOM。
  const key = RADIO_GAIT_KEYS[st.gait] || '';
  const height = typeof st.height === 'number' ? st.height : null;
  if (key === radioGaitSeen && height === radioHeightSeen) return;
  radioGaitSeen = key;
  radioHeightSeen = height;
  if (key) markGait(key);
  if (height !== null) {
    const crawl = height < 0;
    document.querySelectorAll('[data-height]').forEach((b) => {
      b.classList.toggle('active',
        crawl ? b.dataset.height === 'crawl' : b.dataset.height === 'normal');
    });
  }
  paintPickers();
}

// 2.4G 没有网关遥测时，本地先把起立后的步态/身高菜单亮出来。
function applyRadioPose(name) {
  noteWalkIntent(name);
  // 点过之后先按点的显示，但下一帧要让遥测重新说一次：狗可能没接受这次切换
  // （楼梯步态要地形图配合），那时候亮着新档位就是在骗人。
  radioGaitSeen = '';
  radioHeightSeen = null;
  const wrap = $('stage-wrap');
  if (name === 'estop') {
    app.emergencyLocked = true;
    app.rlStanding = false;
    app.walkMode = null;
    if (wrap) {
      wrap.classList.remove('dog-up');
      wrap.classList.add('dog-prone');
    }
    paintStandButton();
    return;
  }
  if (name === 'unload') {
    app.emergencyLocked = false;
    app.rlStanding = false;
    app.walkMode = null;
    if (wrap) {
      wrap.classList.remove('dog-up');
      wrap.classList.add('dog-prone');
    }
    paintStandButton();
    return;
  }
  if (name === 'sit' || name === 'sit_down') {
    app.rlStanding = false;
    app.walkMode = null;
    if (wrap) {
      wrap.classList.remove('dog-up');
      wrap.classList.add('dog-prone');
    }
    paintStandButton();
    paintWalkButtons();
    closeAccordions();
    return;
  }
  if (name === 'stand_up' || name === 'stand') {
    app.emergencyLocked = false;
    app.rlStanding = true;
    app.walkMode = null;
    if (wrap) {
      wrap.classList.add('dog-up');
      wrap.classList.remove('dog-prone');
    }
    paintStandButton();
    paintWalkButtons();
    return;
  }
  if (name === 'torque') {
    app.walkMode = 'torque';
    paintWalkButtons();
    return;
  }
  if (name === 'step') {
    app.walkMode = 'step';
    paintWalkButtons();
    return;
  }
  if (name === 'manual' || name === 'auto') {
    app.modePick = name;
    paintModes();
    return;
  }
  if (name === 'height_low' || name === 'height_normal') {
    document.querySelectorAll('[data-height]').forEach((b) => {
      b.classList.toggle('active',
        (name === 'height_low' && b.dataset.height === 'crawl') ||
        (name === 'height_normal' && b.dataset.height === 'normal'));
    });
    paintPickers();
    return;
  }
  const gaits = ['walk', 'slope', 'offroad', 'lwalk', 'mountain', 'silent',
                 'stair', 'stairmulti', 'stair45'];
  if (gaits.indexOf(name) >= 0) {
    app.gait = name;
    markGait(name);
    paintPickers();
  }
}

// 楼梯那三档长在「上下楼踏面」菜单里，是**必选一项**的设置：上楼时到底用哪种，
// 任何时候都得有个答案，所以有默认值、也不会被遥测灭成一个都不亮。
// active 表示「狗现在真在这个步态」，pick 表示「上楼就用这个」，两者分开记。
const STAIR_GAITS = ['stair', 'stairmulti', 'stair45'];

function markStairPick(key) {
  if (STAIR_GAITS.indexOf(key) < 0) return;
  app.stairPick = key;
  document.querySelectorAll('#stair-row [data-gait]').forEach((b) => {
    b.classList.toggle('pick', b.dataset.gait === key);
  });
  paintPickers();
}

// 标「狗现在在哪个步态」。遥测报的是楼梯步态时，必选项也跟着挪过去，
// 免得菜单里一个亮 active、另一个亮 pick，看着像选了两个。
function markGait(key) {
  document.querySelectorAll('[data-gait]').forEach((b) => {
    b.classList.toggle('active', b.dataset.gait === key);
  });
  markStairPick(key);
}

// 把当前选的那一档写到收起的菜单标题上（步态 · 常规）。
// 只读 DOM 里已经标好的 active，不另存一份状态：两条链路各有一处在标 active
// （MESH 看遥测、2.4G 看发过什么），再存一份就一定会有一处忘了同步。
const PICKERS = [
  { val: 'acc-val-gait', sel: '[data-gait].active' },
  { val: 'acc-val-stair', sel: '#stair-row [data-gait].pick' },
  { val: 'acc-val-height', sel: '[data-height].active' },
];

function paintPickers() {
  for (const p of PICKERS) {
    const el = $(p.val);
    if (!el) continue;
    const on = document.querySelector(p.sel);
    // 还不知道选的是哪一档（没遥测、这一档里也没点过）时退回菜单名，
    // 不然按钮上是一片空白，操作员不知道那颗是干什么的。
    let text = on ? on.textContent : (el.dataset.label || '');
    // 步态切换要跨运动主机与感知主机按序设置，楼梯还要等狗停稳，可达数秒。
    // 这期间显示旧值加省略号，比直接跳到新值老实。
    if (p.val === 'acc-val-gait' && app.gaitPending) text += '…';
    if (el.textContent !== text) el.textContent = text;
  }
}
app.paintPickers = paintPickers;
app.applyRadioPose = applyRadioPose;

// 这个按钮同时是档位和链路灯：字是走哪条路，颜色是那条路通不通 ——
// 绿=通，黄=不通。以前顶栏另挂一条「2.4G通 网关通 ok/fail rx…」的状态串，
// 遥控时那串东西既占地方又要费神读，而真正要一眼看清的只有通没通。
// 出了问题「为什么不通」由黄条讲（见 paintRadioLink），不占顶栏。
function paintRadioBtn(st0) {
  const btn = $('btn-radio');
  if (!btn) return;
  const radio = app.radioPath === 'radio';
  btn.textContent = radio ? '2.4G' : 'MESH';
  const up = radio ? radioLinkUp(st0) : linkOpen();
  btn.classList.toggle('link-up', up);
  btn.classList.toggle('link-down', !up);
}

// 2.4G 通的判据是「指令到得了运动主机」，不是「射频起来了」。
function radioLinkUp(st0) {
  if (!hasNativeRadio()) return false;
  if (st0 && typeof st0.ready === 'boolean') return st0.ready;
  return nativeRadioLinkOk();
}

function notifyNativeRadio() {
  try {
    window.X30Native.setRadioPath(app.radioPath);
  } catch (e) { /* 网页没有原生桥 */ }
}

function syncNativeRadioPath() {
  const native = nativeRadioPath();
  if (!native || native === app.radioPath) return;
  adoptRadioPath(native, false);
}
app.syncNativeRadioPath = syncNativeRadioPath;

function applyRadioPath(announce) {
  app.radioFallback = app.radioPath === 'radio';
  document.documentElement.classList.toggle('radio-24', radioDirect());
  // 初次绘制不要把原生已选的 2.4G 写回 MESH。只在用户或原生真切了档时通知。
  if (app.radioPath === 'radio') {
    if (hasNativeRadio()) {
      if (app.hasControl) {
        app.hasControl = false;
        send({ t: 'yield' });
      }
      if (announce) {
        showBanner('已切到 2.4G。指令走 G20 数传，不经网关');
      }
      setTimeout(() => {
        if (!radioDirect() || !hasNativeRadio()) return;
        if (nativeRadioLinkOk()) return;
        const st = nativeRadioStatus() || {};
        const why = st.status === 'no-usb-net'
          ? 'G20 还没有 USB 网 192.168.144（射频/对频没起来）。关云卓助手和云深处后再切 2.4G'
          : ('2.4G 还没到运动主机 192.168.1.103（' + (st.status || 'unknown') + '）');
        showBanner(why, 10000);
      }, 2000);
    } else if (announce) {
      showBanner('这台 App 还没有 2.4G 直达，控制仍走 MESH');
    }
    paintRadioLink();
    return;
  }
  // 切回 MESH 要把 2.4G 档留下的空文本换成网关那侧的说法，setLink 负责。
  setLink(linkOpen());
  if (linkOpen()) requestControl();
  if (announce) {
    showBanner(linkOpen()
      ? '已切到 MESH，网关接管'
      : '已选 MESH，等网关连上后自动接管');
  }
  paintRadioBtn();
}

function adoptRadioPath(path, announce) {
  const next = path === 'radio' ? 'radio' : 'mesh';
  const changed = app.radioPath !== next;
  // 切档前先把「狗现在站没站」记下来：两侧各自只知道自己发过什么，而狗 RL 起立后
  // 遥测仍报坐下，谁都从遥测里认不出来。不交接的表现就是切一次档姿态变回趴着，
  // 而且轴会被接手那侧吞掉 —— 狗明明站着却推不动。
  const upright = changed ? handoffPose() : null;
  app.radioPath = next;
  try { window.localStorage.setItem(RADIO_STORE, app.radioPath); } catch (e) { /* 记不住就当次有效 */ }
  if (changed) notifyNativeRadio();
  if (changed && upright !== null) {
    // 交给 2.4G 立刻生效；交给网关要等拿到控制权，所以先存着，claim 时带过去。
    if (next === 'radio') pushPoseToRadio(upright);
    else {
      app.poseHandoff = upright;
      poseHintAt = 0;
      app.poseHintWarned = false;
    }
  }
  applyRadioPath(!!announce);
  renderControl();
  // 切档只改指令走哪条路，不主动断网关：两条链路同时在（平板既连着 WiFi 又开着
  // 2.4G）时，操控走 2.4G、画面和遥测仍可走网关。纯 2.4G 现场网关本来就够不到，
  // 这里的重连会一直失败但无害，机身相机那一路由 X30DogCam 原生直拉。
  if (changed) connect();
  if (window.X30DogCam) window.X30DogCam.onRadioPath();
}
// 当前这一侧对姿态有把握吗？没把握就返回 null，宁可不交接 ——
// 猜错的代价是按「趴下」时狗反而站起来，比多按一次起立严重得多。
function handoffPose() {
  if (radioDirect()) {
    // 2.4G 侧：原生发过起立/趴下，或者收到了狗的遥测，才算有依据。
    const st = nativeRadioStatus() || {};
    return st.poseKnown ? isStandingUi() : null;
  }
  // MESH 侧：网关连着且它够得着狗，那份记忆就是有依据的。
  return linkOpen() && app.alive ? isStandingUi() : null;
}

function pushPoseToRadio(upright) {
  try {
    window.X30Native.radioAdoptPose(!!upright);
  } catch (e) { /* 网页没有原生桥 */ }
}

function setRadioPath(path) {
  adoptRadioPath(path, true);
}
app.setRadioPath = setRadioPath;
app.adoptRadioPath = adoptRadioPath;

// 手持壳连上就要权：关掉原厂 App 不会把权交过来，操作员也不该再点一次。
// 网页控制台仍是观察者，避免笔记本开着就把手柄的权抢走。
function requestControl() {
  if (app.hasControl) return;
  if (radioDirect()) return;
  if (!linkOpen()) {
    radioHint();
    return;
  }
  if (app.holder && app.holder !== app.clientId) {
    showBanner('控制权被 #' + app.holder + ' 占用');
    return;
  }
  send(claimMsg());
}
// 手柄那一层也从这里要权：它自己拼 claim 的话，切档带的姿态就丢了。
app.claimMsg = claimMsg;

// 刚从 2.4G 切过来时把姿态一并告知：那段起立/趴下没经过网关，它记的是旧的。
// 这里**不清掉** app.poseHandoff：这条 claim 可能没被授权（权在别人手上），
// 也可能网关是旧版根本不认这个键。清早了就等于交接丢了，而丢了的表现正是
// 「切回 MESH 左下角又变成起立」。等网关的读数真对上了再算交接完（renderState）。
function claimMsg() {
  const claim = { t: 'claim' };
  if (app.poseHandoff !== null) {
    claim.standing = app.poseHandoff;
    if (poseHintAt === 0) poseHintAt = Date.now();
  }
  return claim;
}

// 旧网关不认 claim 里的 standing，会静默忽略。只更新 App 没更新板子很容易发生，
// 而那时候界面自己知道姿态、网关不知道：按钮按我们知道的显示（见 renderState），
// 但轴仍会被网关吞掉，所以必须说清楚该更新哪一头。
let poseHintAt = 0;

function checkPoseHint(s) {
  if (app.poseHandoff === null) return;
  if (!!s.rl_standing === app.poseHandoff) {
    // 网关改口了（新网关采纳，或操作员自己按了一次起立/趴下），交接到此结束。
    app.poseHandoff = null;
    poseHintAt = 0;
    return;
  }
  // 没拿到控制权时网关本来就不会采纳，这不算旧版，也别急着报。
  if (!app.hasControl) return;
  if (poseHintAt === 0) poseHintAt = Date.now();
  if (Date.now() - poseHintAt < 1500 || app.poseHintWarned) return;
  app.poseHintWarned = true;
  showBanner('板子上的网关还是旧版，切档时的姿态没同步过去。' +
             '请更新网关，否则推杆会被网关吞掉（按钮显示的是实际姿态）', 8000);
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

// 2.4G 档下顶栏不再挂状态串（通没通看按钮颜色）。不通时才需要细节，
// 而那属于「出了事要查」，用黄条讲一次，比在顶栏常驻一行更合用。
// 传进来的是本轮已经读过的那份原生状态：每次读都是一趟同步 Binder，
// 而这条回路按发轴频率在跑。
function paintRadioLink(st0) {
  if (!radioDirect()) return;
  const st = st0 || nativeRadioStatus() || {};
  const text = $('link-text');
  if (text) text.textContent = '';
  paintRadioBtn(st);
  if (!st.ready && hasNativeRadio()) {
    const why = st.status === 'no-usb-net'
      ? 'G20 还没有 USB 网 192.168.144（射频/对频没起来）。关云卓助手和云深处后再切 2.4G'
      : ('2.4G 还没到运动主机 192.168.1.103（' + (st.status || 'unknown') + '）');
    showBanner(why, 0, 'wait');
  }
}

function setLink(online) {
  const chip = $('chip-link');
  if (radioDirect()) {
    paintRadioLink();
  } else {
    chip.classList.toggle('online', online && app.alive);
    // 「网关通」这件事现在由 MESH 按钮的颜色说，这里只留网关连上之后才知道的事：
    // 狗到底接没接通。
    $('link-text').textContent = online
        ? (app.alive ? '已连接' : '狗未接通')
        : (app.radioFallback ? '2.4G' : '重连中');
    paintRadioBtn();
  }
  if ($('btn-settings')) $('btn-settings').classList.toggle('online', online);
  if (!online) {
    document.querySelector('.telemetry').classList.add('stale');
  }
  if (window.X30Settings) window.X30Settings.onLink(online);
}

function renderControl() {
  const btn = $('btn-control');
  btn.classList.toggle('held', app.hasControl);
  btn.textContent = '控制权';

  const tag = $('tag-holder');
  tag.classList.toggle('held', app.hasControl);
  if (app.hasControl) {
    tag.textContent = '控制中';
  } else if (!linkOpen()) {
    tag.textContent = app.radioFallback ? '2.4G' : '未连接';
  } else if (app.holder) {
    tag.textContent = `被 #${app.holder} 占用`;
  } else {
    tag.textContent = '观察者';
  }

  paintRadioBtn();
  updateStickAvailability();
}

const fmt = (v, n = 2) => (typeof v === 'number' ? v.toFixed(n) : '—');

function renderState(s) {
  app.alive = !!s.alive;
  setLink(linkOpen());
  if (linkOpen() && !s.alive && !app.warnedRobotDown && !radioDirect()) {
    app.warnedRobotDown = true;
    showBanner('网关在，但够不着狗。把板子 eth0 网线插回机身口，并确认狗已开机', 15000);
  }
  if (s.alive) app.warnedRobotDown = false;
  // 2.4G 直达后运动遥测改回平板，网关会一直报坐下。不能再用它改口。
  if (!radioDirect()) {
    checkPoseHint(s);
    app.basicState = s.basic_state;
    // 交接还没被网关认下来时宁可信自己：这份认知来自 2.4G 那段我们亲手发出去的
    // 起立/趴下，而网关只是「没见过任何指令」。信网关的表现就是切回 MESH 后
    // 左下角又变成「起立」，再按一次狗会先趴下去。
    app.rlStanding = app.poseHandoff !== null ? app.poseHandoff : !!s.rl_standing;
    app.emergencyLocked = s.basic_state === STATE_EMERGENCY || !!s.emergency_source;
  } else if (s.alive && s.basic_state === STATE_EMERGENCY) {
    app.emergencyLocked = true;
    app.rlStanding = false;
  }
  app.controlMode = typeof s.mode === 'number' ? s.mode : 0;

  document.querySelector('.telemetry').classList.toggle('stale', !s.alive);

  const chipState = $('chip-state');
  if (radioDirect()) {
    chipState.textContent = '2.4G';
    chipState.classList.add('online');
  } else {
    chipState.textContent = s.basic_state_text || '—';
    chipState.classList.toggle('online', s.alive);
  }

  const batt = s.battery || {};
  const battText = batt.valid ? `${batt.level}% · ${fmt(batt.voltage, 1)}V` : '—';
  $('chip-batt').textContent = battText;
  if ($('brand-batt')) {
    $('brand-batt').textContent = battText;
    $('brand-batt').classList.toggle('hidden', !isAppShell);
  }
  $('t-batt').textContent = batt.valid ? batt.level : '—';

  $('t-vx').textContent = fmt(s.vel.x);
  $('t-vy').textContent = fmt(s.vel.y);
  $('t-wz').textContent = fmt(s.vel.yaw);
  $('t-ox').textContent = fmt(s.odom.x);
  $('t-oy').textContent = fmt(s.odom.y);
  if (window.X30Cloud && window.X30Cloud.onPose) {
    // 腿式里程计在 RL 起立后经常 x/y 不动。把速度和 IMU 航向一并交给
    // 点云，轨迹才能在里程计失效时靠积分画出来。
    window.X30Cloud.onPose({
      x: s.odom && s.odom.x,
      y: s.odom && s.odom.y,
      yaw: s.odom && s.odom.yaw,
      vx: s.vel && s.vel.x,
      vy: s.vel && s.vel.y,
      wz: s.vel && s.vel.yaw,
      imuYaw: s.att && s.att.yaw,
      mile: s.mileage_cm,
      source: s.odom_source,
    });
  }
  $('t-yaw').textContent = fmt(s.att.yaw, 1);
  $('t-rp').textContent = `${fmt(s.att.roll, 1)} / ${fmt(s.att.pitch, 1)}`;
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

  const lio = s.lio || {};
  app.lioAligning = radioDirect() ? false : !!lio.aligning;
  if (app.emergencyLocked) {
    showBanner('急停后关节已锁，请先点卸力，再起立');
  } else if (lio.aligning && !radioDirect()) {
    const el = $('banner');
    el.textContent = lio.text || 'LIO 正在对准，请站稳，不要走';
    el.className = 'banner banner-wait';
    el.classList.remove('hidden');
    clearTimeout(bannerTimer);
  } else if ($('banner').classList.contains('banner-wait')) {
    $('banner').classList.add('hidden');
  }

  markGait(s.gait_key);
  document.querySelectorAll('[data-height]').forEach((b) => {
    b.classList.toggle('active',
      (s.height_gear === 0 && b.dataset.height === 'normal') ||
      (s.height_gear < 0 && b.dataset.height === 'crawl'));
  });
  paintPickers();

  // 起立/坐下走的是运动主机自己的轨迹，过渡中再点一次会打断甚至反转。
  const standing = isStandingUi();
  const wrap = $('stage-wrap');
  wrap.classList.toggle('dog-up', standing);
  wrap.classList.toggle('dog-prone', !standing);
  paintStandButton();
  // 趴下了力控意图就作废（可能是别人按的趴下），下次起立后推杆照旧自动起步。
  if (!standing) app.torqueByUser = false;
  if (!radioDirect()) {
    if (s.basic_state === STATE_STEPPING) app.walkMode = 'step';
    else if (s.basic_state === STATE_TORQUE_STANDING) app.walkMode = 'torque';
    else if (!standing) app.walkMode = null;
  }
  paintWalkButtons();
  if (!standing) closeAccordions();

  paintModes();

  updateStickAvailability();
}

let bannerTimer = null;
function showBanner(text, holdMs, kind) {
  const el = $('banner');
  if (!el) return;
  el.textContent = text;
  el.classList.toggle('banner-wait', kind === 'wait');
  el.classList.remove('hidden');
  clearTimeout(bannerTimer);
  if (holdMs === 0) return;
  bannerTimer = setTimeout(() => el.classList.add('hidden'), holdMs || 4000);
}

function paintStandButton() {
  const btn = $('btn-stand');
  if (!btn) return;
  const standBusy = !radioDirect() &&
                    (app.basicState === STATE_SIT_TO_STAND ||
                     app.basicState === STATE_STAND_TO_SIT);
  btn.disabled = standBusy;
  if (app.emergencyLocked) {
    btn.textContent = '卸力';
    btn.classList.add('need');
    btn.classList.remove('on');
    return;
  }
  const standing = isStandingUi();
  btn.textContent = standing ? '趴下' : '起立';
  btn.classList.toggle('on', standing && !standBusy);
  btn.classList.remove('need');
}

app.paintModes = paintModes;
app.paintStandButton = paintStandButton;
app.toggleGas = toggleGas;
app.toggleTelem = toggleTelem;
app.cycleView = cycleView;

function paintModes() {
  const pick = app.modePick;
  const manual = app.controlMode === 0;
  document.querySelectorAll('[data-mode]').forEach((b) => {
    const fromTelem = (manual && b.dataset.mode === 'manual') ||
                      (!manual && b.dataset.mode === 'auto');
    const on = pick ? pick === b.dataset.mode : fromTelem;
    b.classList.toggle('active', on);
  });
}

function toggleGas() {
  const el = $('gas-panel');
  if (!el) return;
  el.classList.toggle('hidden');
  const shown = !el.classList.contains('hidden');
  if ($('btn-gas')) $('btn-gas').classList.toggle('active', shown);
}

function toggleTelem() {
  if (!$('telemetry')) return;
  $('telemetry').classList.toggle('hidden');
  const shown = !$('telemetry').classList.contains('hidden');
  if ($('btn-telem')) $('btn-telem').classList.toggle('active', shown);
}

function updateStickAvailability() {
  // 云台不看狗有没有站起来。没拿到控制权时也让推，推杆会去申请。
  const ptz = stickTarget() === 'ptz';
  const usable = ptz
    ? true
    : radioDirect()
      ? (isStandingUi() && !app.emergencyLocked)
      : (app.hasControl && app.alive && controlChannel() !== null &&
         !app.lioAligning);
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
    if (gp.source === 'g20' || gp.engaged) return gp;
  }
  return touchChannels();
}

let webStickTarget = 'dog';

function g20Live() {
  const gp = window.X30Gamepad && window.X30Gamepad.channels
    ? window.X30Gamepad.channels() : null;
  return !!(gp && gp.source === 'g20');
}

function stickTarget() {
  if (g20Live() && window.X30Gamepad.stickTarget) {
    return window.X30Gamepad.stickTarget();
  }
  return webStickTarget;
}

function updateStickHints(ptz) {
  const left = document.querySelector('#stick-left .stick-hint');
  const right = document.querySelector('#stick-right .stick-hint');
  if (left) left.textContent = ptz ? '变倍' : '前后 / 平移';
  if (right) right.textContent = ptz ? '水平 / 俯仰' : '转向 / 俯仰';
}

function paintStickChip() {
  const el = $('chip-stick');
  if (!el) return;
  const ptz = stickTarget() === 'ptz';
  el.classList.remove('hidden');
  el.textContent = ptz ? '摇杆 · 布控球' : '摇杆 · 狗';
  el.classList.toggle('online', ptz);
  updateStickHints(ptz);
  updateStickAvailability();
}

// 2.4G 下屏幕上那两个摇杆也要能推。实体摇杆由原生自己读通道，这里只送触摸那份，
// 免得同一支杆两边各读一遍、死区不一样打起来。
// 云台在网关那侧，2.4G 够不到，所以摇杆指向云台时什么都不发。
function sendRadioVel(c) {
  const n = window.X30Native;
  if (!n || typeof n.radioVel !== 'function') return;
  if (g20Live() || stickTarget() === 'ptz') return;
  n.radioVel(c.fwd || 0, c.lat || 0, c.turn || 0);
}

let ptzNeedControlAt = 0;
let ptzClaimedAt = 0;

setInterval(() => {
  syncNativeRadioPath();
  paintStickChip();
  const onRadio = radioDirect() && hasNativeRadio();
  // 一轮只读一次原生状态，芯片和姿态同步共用。
  const radioSt = onRadio ? (nativeRadioStatus() || {}) : null;
  if (onRadio) paintRadioLink(radioSt);
  const c = activeChannels();
  if (onRadio) {
    syncRadioStanding(radioSt);
    syncRadioPickers(radioSt);
    sendRadioVel(c);
    return;
  }
  if (stickTarget() === 'ptz') {
    const moving = !!(c.engaged || c.fwd || c.lat || c.turn || c.tilt ||
                      c.look);
    if (!app.hasControl) {
      if (radioOnly() || !linkOpen()) return;
      const now = Date.now();
      if (moving && now - ptzClaimedAt > 1500) {
        ptzClaimedAt = now;
        requestControl();
      }
      if (moving && now - ptzNeedControlAt > 4000) {
        ptzNeedControlAt = now;
        if (app.holder && app.holder !== app.clientId) {
          showBanner('控制权被占用，转不了云台');
        } else {
          showBanner('正在申请控制权以转云台');
        }
      }
      return;
    }
    send({ t: 'vel', vx: 0, vy: 0, wz: 0 });
    const look = typeof c.look === 'number' ? c.look : c.tilt;
    // 通道约定上推为正；协议 tilt 也是上为正。海康 ISAPI 同样上为正。
    // 这里再取负会把俯仰拧反。turn 左为正、pan 右为正，所以水平仍取负。
    send({ t: 'ptz', pan: -c.turn, tilt: look, zoom: c.fwd });
    return;
  }
  if (!app.hasControl) return;
  const channel = controlChannel();
  if (channel === 'vel') {
    if (app.lioAligning) {
      send({ t: 'vel', vx: 0, vy: 0, wz: 0 });
      return;
    }
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
  if (!linkOpen()) {
    radioHint();
    return;
  }
  if (app.hasControl) {
    app.radioFallback = true;
    send({ t: 'yield' });
  } else {
    app.radioFallback = false;
    send(claimMsg());
  }
});

function toggleRadioPath() {
  try {
    if (window.X30Native && window.X30Native.toggleRadioPath) {
      window.X30Native.toggleRadioPath();
      return;
    }
  } catch (e) { /* 网页没有原生桥 */ }
  setRadioPath(app.radioPath === 'radio' ? 'mesh' : 'radio');
}
app.toggleRadioPath = toggleRadioPath;
if ($('btn-radio')) {
  $('btn-radio').addEventListener('click', (e) => {
    e.preventDefault();
    e.stopPropagation();
    toggleRadioPath();
  });
}

// 急停不检查控制权，任何客户端任何时候都能按。
$('btn-estop').addEventListener('click', () => {
  syncNativeRadioPath();
  if (radioDirect() && hasNativeRadio()) {
    nativeRadioCmd('estop');
    applyRadioPose('estop');
  }
  send({ t: 'cmd', name: 'estop' });
  app.emergencyLocked = true;
  app.rlStanding = false;
  const wrap = $('stage-wrap');
  wrap.classList.remove('dog-up');
  wrap.classList.add('dog-prone');
  paintStandButton();
  showBanner('急停后关节已锁，请先点卸力，再起立');
});

function radioCmdFromEl(el) {
  if (!el || !el.dataset) return '';
  if (el.dataset.cmd) {
    if (el.dataset.cmd === 'stand') {
      if (app.emergencyLocked) return 'unload';
      // 明确发起立或趴下。发「stand」会翻转，狗已经站着时页面按钮就把狗按趴。
      if (radioDirect()) return nativeRadioStanding() ? 'sit_down' : 'stand_up';
      return isStandingUi() ? 'sit_down' : 'stand_up';
    }
    return el.dataset.cmd;
  }
  if (el.dataset.gait) return el.dataset.gait;
  if (el.dataset.height === 'crawl') return 'height_low';
  if (el.dataset.height) return 'height_normal';
  if (el.dataset.mode && el.dataset.mode !== 'assist') return el.dataset.mode;
  return '';
}

function fireRadioFromEl(el) {
  const name = radioCmdFromEl(el);
  if (!name || !nativeRadioCmd(name)) return '';
  const st = nativeRadioStatus() || {};
  if (!st.ready) {
    showBanner('2.4G 已点「' + name + '」，链路还没通（' + (st.status || 'off') + '）', 5000);
  }
  applyRadioPose(name);
  markPending(el);
  return name;
}

function guarded(fn) {
  return (ev) => {
    syncNativeRadioPath();
    if (radioDirect() && hasNativeRadio()) {
      if (fireRadioFromEl(ev && ev.currentTarget)) return;
      radioHint();
      return;
    }
    if (radioOnly() || !linkOpen()) { radioHint(); return; }
    if (!app.hasControl) { showBanner('请先申请控制权'); return; }
    fn(ev);
  };
}

function markPending(el) {
  if (!el) return;
  el.classList.add('pending');
  clearTimeout(el._pendingTimer);
  el._pendingTimer = setTimeout(() => el.classList.remove('pending'), 700);
}

document.querySelectorAll('[data-cmd]').forEach((b) => {
  b.addEventListener('click', guarded(() => {
    let name = b.dataset.cmd;
    if (name === 'stand' && app.emergencyLocked) name = 'unload';
    else if (name === 'stand') name = isStandingUi() ? 'sit_down' : 'stand_up';
    send({ t: 'cmd', name });
    markPending(b);
    if (name === 'step' && app.lioAligning) {
      showBanner('LIO 还在对准，请站稳，不要走', 4000);
      return;
    }
    if (name === 'torque') {
      app.walkMode = 'torque';
      paintWalkButtons();
    } else if (name === 'step') {
      app.walkMode = 'step';
      paintWalkButtons();
    } else if (name === 'sit_down') {
      app.walkMode = null;
      paintWalkButtons();
    }
  }));
});
document.querySelectorAll('[data-gait]').forEach((b) => {
  b.addEventListener('click', guarded(() => {
    if (app.gaitPending) return;
    // 点了楼梯里的哪一种，必选项就是它 —— 这条是「上楼用哪种」的设置，点了就算，
    // 与狗当前是不是真进了那个步态（楼梯要地形图配合，可能不成）分开记。
    markStairPick(b.dataset.gait);
    setGaitPending(true);
    send({
      t: 'cmd',
      name: 'gait',
      value: b.dataset.gait,
      stair_style: $('stair-style').value,
    });
    markPending(b);
    // 网关最长约 5 秒给结果（含等待静止）。兜底解禁，别把按钮永久锁死。
    setTimeout(() => { if (app.gaitPending) setGaitPending(false); }, 9000);
  }));
});
document.querySelectorAll('[data-height]').forEach((b) => {
  b.addEventListener('click', guarded(() => {
    send({ t: 'cmd', name: 'height', value: b.dataset.height });
    markPending(b);
  }));
});
document.querySelectorAll('[data-mode]').forEach((b) => {
  b.addEventListener('click', (ev) => {
    const mode = b.dataset.mode;
    if (mode === 'assist') {
      app.modePick = 'assist';
      paintModes();
      closeBarPops();
      showBanner('辅助模式本网关未接入，只有原厂 App 支持', 5000);
      return;
    }
    guarded(() => {
      app.modePick = mode;
      send({ t: 'cmd', name: 'mode', value: mode });
      markPending(b);
      paintModes();
      closeBarPops();
    })(ev);
  });
});

$('btn-media').addEventListener('click', () => {
  $('hud-layout').classList.toggle('hidden');
  $('btn-media').classList.toggle('active', !$('hud-layout').classList.contains('hidden'));
});

$('btn-telem').addEventListener('click', () => {
  toggleTelem();
});

$('btn-gas').addEventListener('click', () => {
  toggleGas();
});

$('chip-stick').addEventListener('click', () => {
  if (g20Live()) return;
  webStickTarget = webStickTarget === 'ptz' ? 'dog' : 'ptz';
  if (webStickTarget === 'dog' && app.hasControl) {
    send({ t: 'ptz', pan: 0, tilt: 0, zoom: 0 });
  }
  if (webStickTarget === 'ptz' && !app.hasControl) {
    requestControl();
    showBanner(app.holder && app.holder !== app.clientId
      ? '控制权被占用，转不了云台'
      : '已申请控制权，推杆转云台');
  }
  paintStickChip();
});

$('btn-sticks').addEventListener('click', () => {
  $('hud-sticks').classList.toggle('hidden');
  const shown = !$('hud-sticks').classList.contains('hidden');
  $('btn-sticks').classList.toggle('active', shown);
  if (!shown) {
    app.left.x = 0;
    app.left.y = 0;
    app.right.x = 0;
    app.right.y = 0;
    document.querySelectorAll('.stick .knob').forEach((k) => {
      k.style.transform = 'translate(0px, 0px)';
    });
    if (stickTarget() === 'ptz' && app.hasControl) {
      send({ t: 'ptz', pan: 0, tilt: 0, zoom: 0 });
    }
  }
});

const viewLayout = { mode: '1x1', main: 'dog_cam' };

function applyLayout() {
  const stage = $('stage');
  if (isAppShell) viewLayout.mode = '1x1';
  stage.classList.toggle('layout-1x1', viewLayout.mode === '1x1');
  stage.classList.toggle('layout-2x2', viewLayout.mode === '2x2');
  let pip = 0;
  stage.querySelectorAll('.pane').forEach((p) => {
    const isMain = viewLayout.mode === '1x1' && p.dataset.view === viewLayout.main;
    p.classList.toggle('is-main', isMain);
    // 平板壳只要一张背景，不要一主三小。
    p.classList.toggle('is-pip', !isAppShell && viewLayout.mode === '1x1' && !isMain);
    if (!isAppShell && viewLayout.mode === '1x1' && !isMain) {
      p.dataset.pip = String(pip++);
    } else {
      p.removeAttribute('data-pip');
    }
  });
  $('btn-swap').textContent = viewLayout.mode === '1x1' ? '2×2' : '1×1';
  const cloudMain = viewLayout.mode === '1x1' && viewLayout.main === 'cloud';
  // 2×2 四格都在，点云格子还露着，不能按「不是主背景」退订。
  const cloudVisible = viewLayout.mode === '2x2' || cloudMain;
  // 点云菜单只在 1×1 点云背景时有位置。切走或进四宫格就收起来。
  if (!cloudMain) {
    if ($('cloud-ctl')) $('cloud-ctl').classList.add('hidden');
    if ($('btn-cloud-settings')) $('btn-cloud-settings').classList.remove('active');
  }
  if (window.X30Cloud && window.X30Cloud.setWanted) {
    window.X30Cloud.setWanted(cloudVisible);
  }
  syncViewPick();
  // 点小窗放大布控球时，摇杆跟着改控云台。芯片仍可再切回控狗。
  if (!g20Live() &&
      (viewLayout.main === 'ptz_vis' || viewLayout.main === 'ptz_ir')) {
    webStickTarget = 'ptz';
    if (!app.hasControl) requestControl();
  }
  paintStickChip();
  requestAnimationFrame(() => {
    if (window.X30Cloud && window.X30Cloud.resize) window.X30Cloud.resize();
    if (window.X30Media && window.X30Media.onLayout) window.X30Media.onLayout(viewLayout);
    // 2.4G 的机身相机是原生画面，格子挪了要跟着挪。
    if (window.X30DogCam) window.X30DogCam.onStageLayout();
  });
}

$('btn-swap').addEventListener('click', (e) => {
  e.stopPropagation();
  if (isAppShell) return;
  viewLayout.mode = viewLayout.mode === '1x1' ? '2x2' : '1x1';
  applyLayout();
});

const VIEW_CYCLE = ['dog_cam', 'ptz_vis', 'ptz_ir', 'cloud'];

function syncViewPick() {
  const main = viewLayout.main;
  document.querySelectorAll('[data-view-pick]').forEach((b) => {
    b.classList.toggle('active', b.dataset.viewPick === main);
  });
}

function closeBarPops() {
  if ($('hud-mode-pop')) {
    $('hud-mode-pop').classList.add('hidden');
    if ($('btn-mode')) $('btn-mode').classList.remove('active');
  }
  if ($('hud-view-pop')) {
    $('hud-view-pop').classList.add('hidden');
    if ($('btn-view')) $('btn-view').classList.remove('active');
  }
}

function cycleView() {
  const i = VIEW_CYCLE.indexOf(viewLayout.main);
  viewLayout.main = VIEW_CYCLE[(i < 0 ? 0 : i + 1) % VIEW_CYCLE.length];
  viewLayout.mode = '1x1';
  applyLayout();
}

$('btn-mode').addEventListener('click', (e) => {
  e.stopPropagation();
  const pop = $('hud-mode-pop');
  const open = pop.classList.contains('hidden');
  closeBarPops();
  if (open) {
    pop.classList.remove('hidden');
    $('btn-mode').classList.add('active');
    paintModes();
  }
});

$('btn-view').addEventListener('click', (e) => {
  e.stopPropagation();
  const pop = $('hud-view-pop');
  const open = pop.classList.contains('hidden');
  closeBarPops();
  if (open) {
    pop.classList.remove('hidden');
    $('btn-view').classList.add('active');
    syncViewPick();
  }
});
document.querySelectorAll('[data-view-pick]').forEach((b) => {
  b.addEventListener('click', (e) => {
    e.stopPropagation();
    viewLayout.main = b.dataset.viewPick;
    viewLayout.mode = '1x1';
    closeBarPops();
    applyLayout();
  });
});

$('btn-cloud-settings').addEventListener('click', (e) => {
  e.stopPropagation();
  const ctl = $('cloud-ctl');
  const open = ctl.classList.contains('hidden');
  ctl.classList.toggle('hidden', !open);
  $('btn-cloud-settings').classList.toggle('active', open);
});

document.querySelectorAll('#stage .pane').forEach((pane) => {
  pane.addEventListener('click', (e) => {
    if (e.target.closest('button, select, a, input, #cloud-ctl')) return;
    const view = pane.dataset.view;
    if (viewLayout.mode === '2x2') {
      viewLayout.mode = '1x1';
      viewLayout.main = view;
      applyLayout();
      return;
    }
    if (pane.classList.contains('is-pip')) {
      viewLayout.main = view;
      applyLayout();
    }
  });
});

applyLayout();

function closeAccordions() {
  document.querySelectorAll('.acc-pop').forEach((p) => p.classList.add('hidden'));
  document.querySelectorAll('.acc-btn').forEach((b) => b.classList.remove('active'));
}

document.querySelectorAll('.acc-btn').forEach((btn) => {
  btn.addEventListener('click', (e) => {
    e.stopPropagation();
    const pop = $('acc-' + btn.dataset.acc);
    const open = !pop.classList.contains('hidden');
    closeAccordions();
    if (!open) {
      pop.classList.remove('hidden');
      btn.classList.add('active');
    }
  });
});

document.addEventListener('click', (e) => {
  if (!e.target.closest('.acc')) closeAccordions();
  if (!e.target.closest('#bar-mode, #bar-view, .bar-pop')) closeBarPops();
});

document.querySelectorAll('[data-stair]').forEach((b) => {
  b.addEventListener('click', (e) => {
    e.stopPropagation();
    $('stair-style').value = b.dataset.stair;
    document.querySelectorAll('[data-stair]').forEach((x) => {
      x.classList.toggle('active', x.dataset.stair === b.dataset.stair);
    });
    paintPickers();
  });
});

// 切后台时立刻停车。平板锁屏或切 App 后定时器会被节流，
// 指令不再按时送达，机器人可能带着最后一次速度继续走。
document.addEventListener('visibilitychange', () => {
  if (document.hidden && app.hasControl) {
    send({ t: 'release' });
    showBanner('已切至后台，运动已停止');
  }
  // 切去原厂 App 时心跳会被节流，3 秒租约一过权就没了。
  // 手持壳回到前台自动再要一次，否则横幅只剩「请先发送 claim」。
  if (!document.hidden && isAppShell && app.radioPath === 'mesh') requestControl();
  // 点云订阅不要跟着显隐走：切标签、缩窗口、平板分屏都会把
  // document.hidden 置上，退订后再回来要重新点，操作员会以为订不住。
});

// 启动。挂按钮那一大段在本文件更前面，任何一处抛错都会连带把这里整段跳过 ——
// 现场表现是界面停在 HTML 静态默认值、一条指令都发不出去，且完全看不出原因。
// 所以本函数由文件开头的定时器排队执行，不受后面语句成败影响。
function bootstrap() {
  if (booted) return;
  booted = true;
  // 网页、App 指标和气体都默认藏。网页靠按钮打开，App 靠 G20 R1/R2。
  if ($('telemetry')) {
    $('telemetry').classList.add('hidden');
    if ($('btn-telem')) $('btn-telem').classList.remove('active');
  }
  if ($('gas-panel')) {
    $('gas-panel').classList.add('hidden');
    if ($('btn-gas')) $('btn-gas').classList.remove('active');
  }
  if ($('brand-batt')) $('brand-batt').classList.toggle('hidden', !isAppShell);
  paintVerChip();
  syncNativeRadioPath();
  applyRadioPath(false);
  renderControl();
  // 踏面默认是实心（HTML 里就带 active），开机就该显出来。
  paintPickers();

  connect();

  if (window.X30Media) window.X30Media.initMedia(send, showBanner);
  if (window.X30DogCam) window.X30DogCam.init();
  if (window.X30Capture) window.X30Capture.initCapture(showBanner);
  if (window.X30Cloud) window.X30Cloud.initCloud(send);
  if (window.X30Gamepad) window.X30Gamepad.initGamepad(send, showBanner, () => app);
  if (window.X30Settings) window.X30Settings.initSettings(send);
}
bootstrap();
