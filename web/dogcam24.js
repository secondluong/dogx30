// App 原生 RTSP：画面一律钉 WiFi/MESH 网卡。运动走 2.4G 时也不把视频绑到
// ar_net0——球在 10 网、机身在 1 网都能从 MESH/WiFi 到；绑射频口只会把球拉黑。

'use strict';

const DOGCAM_KEY = 'x30.dogcam24.url';
const DOGCAM_DEFAULT = 'rtsp://192.168.1.105:8554/test';
const PTZ_KEY = 'x30.ptz_vis.rtsp';
const PTZ_DEFAULT = 'rtsp://192.168.10.168:554/11';

let lastRect = '';
let playing = false;
let lastErr = '';
let lastBuf = 0;
let wantedNow = false;
let playKeyNow = '';
const idleOrig = {};

function nativeVideo() {
  const n = window.X30Native;
  // WebView 里桥方法的 typeof 常常不是 function。
  try {
    return (n && 'videoStart' in n) ? n : null;
  } catch (e) {
    return null;
  }
}

function stored(key, fallback) {
  try {
    const v = window.localStorage.getItem(key);
    if (v) return v;
  } catch (e) { /* 存不了就用默认值 */ }
  return fallback;
}

function url() {
  return stored(DOGCAM_KEY, DOGCAM_DEFAULT);
}

function ptzUrl() {
  return stored(PTZ_KEY, PTZ_DEFAULT);
}

function remember(key, fallback, next) {
  const v = (next || '').trim();
  try {
    if (v && v !== fallback) window.localStorage.setItem(key, v);
    else window.localStorage.removeItem(key);
  } catch (e) { /* 记不住就当次有效 */ }
}

function setUrl(next) {
  remember(DOGCAM_KEY, DOGCAM_DEFAULT, next);
  if (playing || wantedNow) {
    stop();
    sync();
  }
}

function setPtzUrl(next) {
  const prev = ptzUrl();
  remember(PTZ_KEY, PTZ_DEFAULT, next);
  if ((playing || wantedNow) && ptzUrl() !== prev) {
    stop();
    sync();
  }
}

function radio24() {
  return document.documentElement.classList.contains('radio-24');
}

function visPane() {
  return document.getElementById('pane-ptz-vis');
}

function dogPane() {
  return document.getElementById('pane-video');
}

function mainId() {
  const vis = visPane();
  if (vis && vis.classList.contains('is-main')) return 'ptz_vis';
  const dog = dogPane();
  if (dog && dog.classList.contains('is-main')) return 'dog_cam';
  return '';
}

function pane() {
  return mainId() === 'ptz_vis' ? visPane() : dogPane();
}

function playUrl() {
  if (mainId() === 'ptz_vis') return ptzUrl();
  if (mainId() === 'dog_cam') return url();
  return '';
}

function bindRadio() {
  // 视频/球机属于载荷，不跟运动档位走 2.4G。永远 false → 绑 WiFi/MESH。
  return false;
}

// 2.4G / MESH 都直拉球/机身，socket 始终走 WiFi/MESH。
function wanted() {
  if (!nativeVideo()) return false;
  if (document.hidden) return false;
  const id = mainId();
  return id === 'ptz_vis' || id === 'dog_cam';
}

function playKey() {
  return mainId() + '|' + playUrl() + '|' + (bindRadio() ? 'r' : 'l');
}

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

function markNative(id) {
  const root = document.documentElement;
  if (id) {
    root.classList.add('native-video-on');
    root.setAttribute('data-native-video', id);
  } else {
    root.classList.remove('native-video-on');
    root.removeAttribute('data-native-video');
  }
}

function stop() {
  const n = nativeVideo();
  wantedNow = false;
  playKeyNow = '';
  lastRect = '';
  if (n) n.videoStop();
  playing = false;
  lastErr = '';
  markNative('');
  restoreIdleText();
  handOver();
}

function handOver() {
  if (window.X30Media && window.X30Media.resync) window.X30Media.resync();
}

function startNative() {
  const n = nativeVideo();
  const u = playUrl();
  if ('videoStartOn' in n) n.videoStartOn(u, bindRadio());
  else n.videoStart(u);
}

function sync() {
  const n = nativeVideo();
  if (!n) return;
  if (!wanted()) {
    if (wantedNow) stop();
    return;
  }
  pushRect();
  const id = mainId();
  const had = document.documentElement.classList.contains('native-video-on');
  const same = document.documentElement.getAttribute('data-native-video') === id;
  markNative(id);
  if (!had || !same) handOver();
  const key = playKey();
  if (!wantedNow || playKeyNow !== key) {
    wantedNow = true;
    playKeyNow = key;
    startNative();
  }
  paint();
}

function idleEl() {
  return document.getElementById(
    mainId() === 'ptz_vis' ? 'media-idle-ptz-vis' : 'media-idle');
}

function idleSmall() {
  const idle = idleEl();
  return idle ? idle.querySelector('small') : null;
}

function rememberIdle(id) {
  const el = document.getElementById(id);
  const small = el ? el.querySelector('small') : null;
  if (small && idleOrig[id] === undefined) idleOrig[id] = small.textContent;
}

function restoreIdleText() {
  ['media-idle', 'media-idle-ptz-vis', 'media-idle-ptz-ir'].forEach((id) => {
    const el = document.getElementById(id);
    const small = el ? el.querySelector('small') : null;
    if (small && idleOrig[id]) small.textContent = idleOrig[id];
  });
}

// 热成像仍由网关 MediaMTX / 桌面 WHEP 编排；App 主画面走原生白光。
// 2.4G 运动档不切断 MESH，热成像仍可从网关侧拿到。
function paintPtz(/* on */) {
  const id = 'media-idle-ptz-ir';
  rememberIdle(id);
  const el = document.getElementById(id);
  const small = el ? el.querySelector('small') : null;
  if (!small || !idleOrig[id]) return;
  if (small.textContent !== idleOrig[id]) small.textContent = idleOrig[id];
}

function paint() {
  const idle = idleEl();
  if (!idle || !wantedNow) return;
  idle.classList.toggle('hidden', playing);
  if (playing) return;
  const small = idleSmall();
  if (!small) return;
  const prefix = '直连拉流失败：';
  small.textContent = lastErr ? (prefix + lastErr)
                             : ('正在从 ' + playUrl() + ' 拉流…');
}

function onState(st) {
  const s = st || {};
  playing = !!s.playing;
  lastErr = s.err || '';
  lastBuf = typeof s.buf === 'number' ? s.buf : 0;
  paint();
}

function status() {
  if (!nativeVideo()) return '';
  if (!wantedNow) {
    if (mainId() === 'ptz_vis') return '未在拉流。';
    if (!radio24()) return '未在拉流（MESH 下这一路不是当前主画面）。';
    return '未在拉流（机身相机不是当前主画面）。';
  }
  if (!playing) return lastErr ? ('拉流失败：' + lastErr) : '正在连接…';
  return '正在放，本机缓冲 ' + Math.round(lastBuf) + ' ms'
    + (lastBuf > 400 ? '（偏大，正在快放追）' : '（延迟主要在上游）');
}

function onStageLayout() {
  sync();
}

function onRadioPath() {
  paintPtz();
  sync();
}

function init() {
  if (!nativeVideo()) return;
  rememberIdle('media-idle');
  rememberIdle('media-idle-ptz-vis');
  rememberIdle('media-idle-ptz-ir');
  window.addEventListener('resize', () => { lastRect = ''; pushRect(); });
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) stop();
    else sync();
  });
  window.setInterval(() => {
    if (wantedNow) pushRect();
    else onRadioPath();
  }, 1000);
  onRadioPath();
}

window.X30DogCam = {
  init, onStageLayout, onRadioPath, onState, url, setUrl, setPtzUrl, ptzUrl,
  stop, status,
};
