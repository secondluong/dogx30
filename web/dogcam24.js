// 2.4G 链路下的机身相机画面。
//
// 为什么这一路不能和 MESH 共用同一套代码：2.4G 是**遥控器与狗直连** —— 接收机挂在
// 机身交换机上，网关开发板根本不在这条链路上。所以拿不到网关下发的媒体计划，
// 也够不到 MediaMTX，网页那条 WebRTC 整条链都不成立。狗自己只提供 RTSP
// （感知主机 8554），而 WebView 放不了 RTSP —— 这就是 2.4G 下一直没画面的原因。
//
// 办法是让安卓原生解码，画面垫在 WebView 底下。网页这边只做三件事：
// 决定什么时候要放、把画面格子的屏幕矩形报给原生、把那块区域的背景透出去。
// 反过来原生把播放状态报回来，占位图上就能显示真实原因而不是干等。

'use strict';

// 机身相机地址。厂家拓扑里感知主机是 192.168.1.105，RTSP 由它把 USB 相机转出来
// （见 docs/media-architecture.md 第十、十一节）。允许在设置里改：现场换过相机或
// 改过端口时，不该为此重新编包。
const DOGCAM_KEY = 'x30.dogcam24.url';
const DOGCAM_DEFAULT = 'rtsp://192.168.1.105:8554/test';

// 矩形有变化才下发。原生那边一次 setRect 会重排 SurfaceView，白扔没必要的重排
// 会让画面闪。
let lastRect = '';
let playing = false;
let lastErr = '';
let lastBuf = 0;
let wantedNow = false;
let idleText = '';

function nativeVideo() {
  const n = window.X30Native;
  return n && typeof n.videoStart === 'function' ? n : null;
}

function url() {
  try {
    const v = window.localStorage.getItem(DOGCAM_KEY);
    if (v) return v;
  } catch (e) { /* 存不了就用默认值 */ }
  return DOGCAM_DEFAULT;
}

function setUrl(next) {
  const v = (next || '').trim();
  try {
    if (v && v !== DOGCAM_DEFAULT) window.localStorage.setItem(DOGCAM_KEY, v);
    else window.localStorage.removeItem(DOGCAM_KEY);
  } catch (e) { /* 记不住就当次有效 */ }
  // 地址变了要重开，否则还在放旧地址。
  if (playing || wantedNow) {
    stop();
    sync();
  }
}

function radio24() {
  return document.documentElement.classList.contains('radio-24');
}

function pane() {
  return document.getElementById('pane-video');
}

// 只在「2.4G + 机身相机正当主画面」时放。切到布控球或点云就停 ——
// 那两路在网关那侧，2.4G 下本来就没有。
function wanted() {
  if (!radio24() || !nativeVideo()) return false;
  if (document.hidden) return false;
  const p = pane();
  return !!p && p.classList.contains('is-main');
}

// 报给原生的是**设备像素**下的矩形：网页这边一律 CSS 像素，原生那边是像素，
// 中间差一个 devicePixelRatio。在这里换算掉，原生就不必去猜网页的缩放。
function rectOf(p) {
  const r = p.getBoundingClientRect();
  const s = window.devicePixelRatio || 1;
  return {
    x: Math.round(r.left * s),
    y: Math.round(r.top * s),
    w: Math.round(r.width * s),
    h: Math.round(r.height * s),
  };
}

function pushRect() {
  const n = nativeVideo();
  const p = pane();
  if (!n || !p) return;
  const r = rectOf(p);
  if (r.w <= 0 || r.h <= 0) return;
  const key = r.x + ',' + r.y + ',' + r.w + ',' + r.h;
  if (key === lastRect) return;
  lastRect = key;
  n.videoRect(r.x, r.y, r.w, r.h);
}

function stop() {
  const n = nativeVideo();
  wantedNow = false;
  lastRect = '';
  if (n) n.videoStop();
  playing = false;
  lastErr = '';
  document.documentElement.classList.remove('native-video-on');
  // 先把占位图的话还原，否则切回 MESH 还挂着 2.4G 的提示；再交回 media.js，
  // 它接手后自己会决定这一格是放画面还是继续显占位。
  const small = idleSmall();
  if (small && idleText) small.textContent = idleText;
  handOver();
}

// 机身相机这一路归谁拉，是「原生」和「网关 WebRTC」二选一。归属一变就得让
// media.js 重算，否则切到 2.4G 时它那条 WebRTC 还挂着：同一只相机两条流并行，
// 白占本来就窄的 2.4G，还要抢同一块占位图。
function handOver() {
  if (window.X30Media && window.X30Media.resync) window.X30Media.resync();
}

function sync() {
  const n = nativeVideo();
  if (!n) return;
  if (!wanted()) {
    if (wantedNow) stop();
    return;
  }
  pushRect();
  // 背景要立刻透出去：原生画面已经垫在下面，网页再铺一层黑就白拉了。
  const had = document.documentElement.classList.contains('native-video-on');
  document.documentElement.classList.add('native-video-on');
  if (!had) handOver();
  // 只在状态翻转时开一次。切布局、切档都会走到这里，反复 videoStart
  // 会把正在放的流打断。断流重试由原生自己带退避做，不靠这里轮询。
  if (!wantedNow) {
    wantedNow = true;
    n.videoStart(url());
  }
  paint();
}

function idleSmall() {
  const idle = document.getElementById('media-idle');
  return idle ? idle.querySelector('small') : null;
}

// 布控球和热成像挂在网关那侧的 192.168.10.0/24，2.4G 根本到不了。
// 不说清楚的话，那两格停在「未接通时会停在这里」，看着像设备坏了。
const PTZ_IDLES = ['media-idle-ptz-vis', 'media-idle-ptz-ir'];
const ptzText = new Map();

function paintPtz(on) {
  for (const id of PTZ_IDLES) {
    const el = document.getElementById(id);
    const small = el ? el.querySelector('small') : null;
    if (!small) continue;
    if (!ptzText.has(id)) ptzText.set(id, small.textContent);
    const next = on ? '2.4G 下没有这一路：布控球在网关那侧，要看它请切 MESH'
                    : ptzText.get(id);
    if (small.textContent !== next) small.textContent = next;
  }
}

// 占位图上写清楚卡在哪。2.4G 下画面这条链就三段（网卡绑定、RTSP、解码），
// 原生把断在哪一段报回来就直接写上去，不用去翻日志。
function paint() {
  const idle = document.getElementById('media-idle');
  if (!idle || !wantedNow) return;   // 不该我管时交回 media.js
  idle.classList.toggle('hidden', playing);
  if (playing) return;
  const small = idleSmall();
  if (!small) return;
  small.textContent = lastErr ? ('2.4G 直连拉流失败：' + lastErr)
                             : ('正在从 ' + url() + ' 拉流…');
}

// 原生回调：{playing:bool, err:string, buf:number}
function onState(st) {
  const s = st || {};
  playing = !!s.playing;
  lastErr = s.err || '';
  lastBuf = typeof s.buf === 'number' ? s.buf : 0;
  paint();
}

// 给设置面板看的一行。buf 是播放器里「已收到但还没放」的那段，也就是本机贡献的
// 延迟：它接近 0 而画面仍然慢，说明慢在上游（相机转码、链路排队），
// 调客户端没用。分清这两种情况，比继续猜有用得多。
function status() {
  if (!nativeVideo()) return '';
  if (!radio24()) return 'MESH 链路下这一路走网关，不用原生拉流。';
  if (!wantedNow) return '未在拉流（机身相机不是当前主画面）。';
  if (!playing) return lastErr ? ('拉流失败：' + lastErr) : '正在连接…';
  return '正在放，本机缓冲 ' + Math.round(lastBuf) + ' ms'
    + (lastBuf > 400 ? '（偏大，正在快放追）' : '（延迟主要在上游）');
}

function onStageLayout() {
  sync();
}

function onRadioPath() {
  // 这一条和主画面是谁无关：只要在 2.4G，布控球那两格就该如实说不可用。
  paintPtz(radio24() && !!nativeVideo());
  sync();
}

function init() {
  if (!nativeVideo()) return;
  const small = idleSmall();
  if (small) idleText = small.textContent;
  window.addEventListener('resize', () => { lastRect = ''; pushRect(); });
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) stop();
    else sync();
  });
  // 矩形会因为横幅、菜单展开等原因变，而这些地方不会都去调 onStageLayout。
  // 另外切档不一定经过 adoptRadioPath（开机时是直接 applyRadioPath 的）。
  // 一秒看一次、只在真变了才下发，成本可以忽略。
  window.setInterval(() => {
    if (wantedNow) pushRect();
    else onRadioPath();
  }, 1000);
  onRadioPath();
}

window.X30DogCam = {
  init, onStageLayout, onRadioPath, onState, url, setUrl, stop, status,
};
