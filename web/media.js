// 视频与对讲。协议见 docs/app-protocol.md，设计依据见 docs/media-architecture.md。
//
// 三件事：探测本机能解什么编码并上报、按网关下发的「媒体计划」拉流、按住说话。
// 视频不经过网关，直接从 MediaMTX 走 WebRTC 拉，网关只负责编排。

'use strict';

const media = {
  plan: null,
  main: null,          // 当前主视图的 source id
  layout: null,        // 遥控台当前布局，App 只拉 layout.main 那一路
  pcs: new Map(),      // sourceId -> RTCPeerConnection
  talkPc: null,
  talkStream: null,
  caps: { h264: true, h265: false },
};

const playingPath = new Map();
const lastPlayErr = new Map();

// 名字不能叫 isAppShell：本文件和 app.js 共用一个全局作用域，app.js 里有同名
// const，重名会让 app.js 整份解析失败（SyntaxError），一条指令都发不出去。
function inAppShell() {
  return typeof document !== 'undefined' &&
    document.documentElement.classList.contains('shell-app');
}

// Chrome 把 H.265 列进 getCapabilities，不代表 WebRTC 能解出色度。
// 桌面浏览器（Linux / 不少 Windows 核显）常见结果就是白光整幅发绿，
// 同一台球机的 H.264 热成像子码流却是正常的。安卓壳走 SoC 硬解，可以继续用。
function webH265Ok() {
  return inAppShell();
}

// ---------------------------------------------------------------------------
// 编解码能力探测
// ---------------------------------------------------------------------------

// 必须实测，不能看 UserAgent 猜：H.265 在 WebRTC 里只有硬件解码、没有软解兜底，
// 同一个 Chrome 版本在不同 SoC 上结论可能不同。猜错的代价是黑屏。
function probeCodecsSync() {
  const out = { h264: false, h265: false };
  if (typeof RTCRtpReceiver === 'undefined' ||
      !RTCRtpReceiver.getCapabilities) {
    // 老到连这个 API 都没有的浏览器，只能假定 H.264 —— 它是 WebRTC 的通用底线。
    return { h264: true, h265: false };
  }
  const caps = RTCRtpReceiver.getCapabilities('video');
  if (!caps || !caps.codecs) return { h264: true, h265: false };
  for (const c of caps.codecs) {
    const m = (c.mimeType || '').toLowerCase();
    if (m === 'video/h264') out.h264 = true;
    if (m === 'video/h265' || m === 'video/hevc') out.h265 = true;
  }
  return out;
}

// getCapabilities 说支持不代表真能解（可能列了但硬件不行），
// 有 mediaCapabilities 时再确认一遍。
async function confirmH265() {
  if (!navigator.mediaCapabilities || !navigator.mediaCapabilities.decodingInfo) {
    return null;  // 无法确认，沿用上面的结论
  }
  try {
    const r = await navigator.mediaCapabilities.decodingInfo({
      type: 'webrtc',
      video: {
        contentType: 'video/H265',
        width: 1920, height: 1080, bitrate: 3000000, framerate: 25,
      },
    });
    return !!r.supported;
  } catch (e) {
    return null;
  }
}

async function reportCaps(sendFn) {
  media.caps = probeCodecsSync();
  const confirmed = await confirmH265();
  if (confirmed !== null) media.caps.h265 = confirmed;
  // 桌面端宁可退到 H.264 子码流，也不要把一张绿图当「已支持」。
  if (!webH265Ok()) media.caps.h265 = false;

  sendFn({ t: 'media_caps', h264: media.caps.h264, h265: media.caps.h265 });

  const el = document.getElementById('codec-note');
  if (el) {
    if (media.caps.h265) {
      el.textContent = 'H.265 可用';
    } else if (!webH265Ok()) {
      el.textContent = '桌面浏览器 H.265 容易整幅发绿，白光改走 H.264';
    } else {
      el.textContent = 'H.265 不可用，将使用 H.264（画质相同则占用约两倍带宽）';
    }
    el.classList.toggle('warn', !media.caps.h265);
  }
}

// ---------------------------------------------------------------------------
// WHEP 拉流
// ---------------------------------------------------------------------------

// MediaMTX 用 WHEP：把 offer 以 application/sdp POST 过去，回来就是 answer。
async function whepPlay(baseUrl, path, videoEl) {
  const pc = new RTCPeerConnection({ iceServers: [] });
  pc.addTransceiver('video', { direction: 'recvonly' });
  pc.addTransceiver('audio', { direction: 'recvonly' });

  const stream = new MediaStream();
  pc.ontrack = (ev) => {
    stream.addTrack(ev.track);
    videoEl.srcObject = stream;
  };

  const offer = await pc.createOffer();
  await pc.setLocalDescription(offer);

  // 等 ICE 收集完再发。这是个封闭内网，候选很少，收集很快；
  // 用 trickle 反而要多维护一条信令通路，不值得。
  await new Promise((resolve) => {
    if (pc.iceGatheringState === 'complete') return resolve();
    const check = () => {
      if (pc.iceGatheringState === 'complete') {
        pc.removeEventListener('icegatheringstatechange', check);
        resolve();
      }
    };
    pc.addEventListener('icegatheringstatechange', check);
    setTimeout(resolve, 1500);  // 兜底，别卡死在这里
  });

  const res = await fetch(`${baseUrl}/${path}/whep`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/sdp' },
    body: pc.localDescription.sdp,
  });
  if (!res.ok) {
    pc.close();
    throw new Error(`WHEP ${res.status}`);
  }
  const answer = await res.text();
  await pc.setRemoteDescription({ type: 'answer', sdp: answer });
  return pc;
}

// 网页一主三小 / 2×2 会同时拉这三路。App 只有一张底图，syncTiles 只留当前主画面。
const VIDEO_TILES = [
  { id: 'dog_cam', video: 'media-video', idle: 'media-idle', fallback: 'dog_cam_main' },
  { id: 'ptz_vis', video: 'media-video-ptz-vis', idle: 'media-idle-ptz-vis', fallback: 'ptz_vis_sub' },
  { id: 'ptz_ir', video: 'media-video-ptz-ir', idle: 'media-idle-ptz-ir', fallback: 'ptz_ir_sub' },
];

function stopSource(id) {
  const pc = media.pcs.get(id);
  if (pc) {
    pc.close();
    media.pcs.delete(id);
  }
  playingPath.delete(id);
  const tile = VIDEO_TILES.find((t) => t.id === id);
  if (!tile) return;
  const el = document.getElementById(tile.video);
  if (el) el.srcObject = null;
}

function stopAll() {
  for (const id of Array.from(media.pcs.keys())) stopSource(id);
}

function setIdle(id, on) {
  const el = document.getElementById(id);
  if (el) el.classList.toggle('hidden', !on);
}

function pathFor(tile, plan) {
  // 桌面端点成大屏时网关会改发 H.265 主码流，色度解不出来就整幅发绿。
  // 小窗之所以正常，正是因为它走子码流。大屏也锁在子码流上。
  if (!webH265Ok() && tile.fallback && tile.id !== 'dog_cam') {
    return tile.fallback;
  }
  const src = plan.sources.find((s) => s.id === tile.id);
  if (src && src.available && src.path) return src.path;
  return tile.fallback;
}

function playTile(plan, tile, showBanner) {
  const videoEl = document.getElementById(tile.video);
  if (!videoEl || !plan.webrtc_base) return;
  const path = pathFor(tile, plan);
  if (media.pcs.has(tile.id) && playingPath.get(tile.id) === path) return;
  stopSource(tile.id);
  playingPath.set(tile.id, path);
  setIdle(tile.idle, false);
  whepPlay(plan.webrtc_base, path, videoEl)
    .then((pc) => {
      media.pcs.set(tile.id, pc);
      lastPlayErr.delete(tile.id);
    })
    .catch((e) => {
      lastPlayErr.set(tile.id, e.message);
      setIdle(tile.idle, true);
      stopSource(tile.id);
      playingPath.delete(tile.id);
      if (tile.id === 'dog_cam' && showBanner) {
        showBanner(`机身相机拉流失败：${e.message}`, 6000);
      }
    });
}

// 没画面时界面上只有「等待拉流」四个字，看不出是没收到计划、没选到这一路、
// 还是拉流被拒 —— 现场只能去翻网关日志，而网关往往不在手边。点占位图即报卡点。
// 入口只在没画面时才存在（有画面时占位图是隐藏的），所以不会常驻界面。
function whyNoVideo(tileId) {
  const plan = media.plan;
  if (!plan) {
    return '还没收到网关下发的媒体计划。先看顶栏链路，网关不通就不会有画面';
  }
  if (!plan.webrtc_base) {
    return '媒体计划里没有拉流地址：网关 media.json 的 webrtc_base 没填';
  }
  const main = (media.layout && media.layout.main) || plan.main;
  if (inAppShell() && main !== tileId) {
    return 'App 只拉当前背景那一路，现在的背景不是它。用顶栏「背景」切过来';
  }
  const src = (plan.sources || []).find((s) => s.id === tileId);
  const name = (src && src.name) || tileId;
  if (!src) return '网关没配这一路视频源（media.json 的 sources 里没有 ' + tileId + '）';
  if (!src.available) return name + '不可用：' + (src.reason || '网关没给原因');
  const err = lastPlayErr.get(tileId);
  if (err) return name + ' 拉流被拒：' + err + '（拉流地址 ' + plan.webrtc_base + '）';
  if (!media.pcs.has(tileId)) {
    return name + ' 还没开始拉流（拉流地址 ' + plan.webrtc_base + '）';
  }
  // 会话建起来了却一帧都没有，几乎只剩一种原因：网关自己也没从相机拿到流。
  return name + ' 会话已建立但没有画面，通常是网关到相机那一段不通。'
       + '在网关上查 curl -s http://127.0.0.1:9997/v3/paths/list';
}

function renderMediaPanel(plan, showBanner) {
  const panel = document.getElementById('media-list');
  if (!panel) return;

  panel.innerHTML = '';

  for (const s of plan.sources) {
    const row = document.createElement('div');
    row.className = 'media-row' + (s.id === plan.main ? ' is-main' : '');

    const btn = document.createElement('button');
    btn.className = 'btn media-pick';
    btn.textContent = s.name;
    const canPick = s.available ||
      (s.reason && s.reason.indexOf('选为主视图') !== -1);
    btn.disabled = !canPick;
    btn.onclick = () => selectMain(s.id);
    row.appendChild(btn);

    const tag = document.createElement('span');
    tag.className = 'media-tag';
    if (!s.available) {
      tag.textContent = s.reason || '不可用';
      tag.classList.add('warn');
    } else {
      tag.textContent = `${s.quality === 'main' ? '全码率' : '缩略图'} · ` +
                        `${s.codec.toUpperCase()} · ${s.label || ''} · ${s.kbps} kbps`;
      if (s.downgraded) {
        tag.textContent += ' · 已降级';
        tag.classList.add('warn');
      }
    }
    row.appendChild(tag);
    panel.appendChild(row);
  }

  const budget = document.getElementById('media-budget');
  if (budget) {
    budget.textContent = `合计 ${plan.total_kbps} / ${plan.budget_kbps} kbps`;
    budget.classList.toggle('warn', !!plan.over_budget);
  }
  if (plan.over_budget && showBanner) {
    showBanner('视频码率超出链路预算，请降低相机码率设置', 9000);
  }

  if (!plan.main && sendRef) {
    const dog = plan.sources.find((s) => s.id === 'dog_cam');
    if (dog) {
      sendRef({ t: 'media_select', id: 'dog_cam' });
      return;
    }
  }

  syncTiles(plan, showBanner);
}

// 网关推来的媒体计划。plan 消息本身就是计划，见 docs/app-protocol.md。
// media.plan 是对讲、切布局、切后台回来重拉都要读的那份状态，必须在这里存下。
function onMediaPlan(plan, showBanner) {
  if (!plan || !Array.isArray(plan.sources)) return;
  media.plan = plan;
  media.main = plan.main || null;
  renderMediaPanel(plan, showBanner);
}

// App 只有一张底图，没露出来的路不要占带宽。网页一主三小 / 2×2 仍同时拉。
function wantedTiles(plan) {
  if (!inAppShell()) return VIDEO_TILES;
  const main = (media.layout && media.layout.main) || plan.main;
  if (!main || main === 'cloud') return [];
  return VIDEO_TILES.filter((t) => t.id === main);
}

function syncTiles(plan, showBanner) {
  const wanted = new Set(wantedTiles(plan).map((t) => t.id));
  for (const tile of VIDEO_TILES) {
    if (wanted.has(tile.id)) {
      playTile(plan, tile, showBanner);
    } else {
      stopSource(tile.id);
      setIdle(tile.idle, true);
    }
  }
}

function onLayout(layout) {
  media.layout = layout || null;
  if (media.plan) syncTiles(media.plan, talkBanner);
  if (!layout || !layout.main || layout.main === 'cloud') return;
  // 桌面布控球只看 H.264 子码流，不要占掉全码率槽位，否则 App 反而拿不到 1080p。
  if (!webH265Ok() && (layout.main === 'ptz_vis' || layout.main === 'ptz_ir')) {
    return;
  }
  if (sendRef) sendRef({ t: 'media_select', id: layout.main });
}

let sendRef = null;
function selectMain(id) {
  if (sendRef) sendRef({ t: 'media_select', id });
}

// ---------------------------------------------------------------------------
// 对讲：按住说话
// ---------------------------------------------------------------------------

// 做成按住说话而不是常开：工地噪音大，常开既容易啸叫也白占带宽。
async function talkStart(showBanner) {
  if (media.talkPc || !media.plan) return;
  try {
    media.talkStream = await navigator.mediaDevices.getUserMedia({
      audio: {
        echoCancellation: true,   // 没有 AEC，狗那侧的喇叭会把话音采回来形成啸叫
        noiseSuppression: true,
        autoGainControl: true,
      },
    });
  } catch (e) {
    if (showBanner) showBanner(`拿不到麦克风：${e.message}`, 6000);
    return;
  }

  const pc = new RTCPeerConnection({ iceServers: [] });
  for (const track of media.talkStream.getAudioTracks()) {
    pc.addTrack(track, media.talkStream);
  }

  try {
    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    const res = await fetch(`${media.plan.webrtc_base}/talkback/whip`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/sdp' },
      body: pc.localDescription.sdp,
    });
    if (!res.ok) throw new Error(`WHIP ${res.status}`);
    await pc.setRemoteDescription({ type: 'answer', sdp: await res.text() });
    media.talkPc = pc;
  } catch (e) {
    pc.close();
    talkStop();
    if (showBanner) showBanner(`对讲失败：${e.message}`, 6000);
  }
}

function talkStop() {
  if (media.talkPc) {
    media.talkPc.close();
    media.talkPc = null;
  }
  if (media.talkStream) {
    media.talkStream.getTracks().forEach((t) => t.stop());
    media.talkStream = null;
  }
}

// ---------------------------------------------------------------------------
// 接线
// ---------------------------------------------------------------------------

let talkBanner = null;

function setTalk(on, showBanner) {
  const talkBtns = document.querySelectorAll('.btn-talk');
  talkBtns.forEach((b) => b.classList.toggle('active', on));
  if (on) talkStart(showBanner || talkBanner);
  else talkStop();
}

function initMedia(sendFn, showBanner) {
  sendRef = sendFn;
  talkBanner = showBanner;
  reportCaps(sendFn);

  VIDEO_TILES.forEach((tile) => {
    const ph = document.getElementById(tile.idle);
    if (!ph) return;
    // 不冒泡：占位图罩在画面格子上，格子本身点了会切主视图。
    ph.addEventListener('click', (e) => {
      e.preventDefault();
      e.stopPropagation();
      if (showBanner) showBanner(whyNoVideo(tile.id), 12000);
    });
  });

  const talkBtns = document.querySelectorAll('.btn-talk');
  talkBtns.forEach((talkBtn) => {
    const down = (e) => {
      e.preventDefault();
      e.stopPropagation();
      setTalk(true, showBanner);
    };
    const up = (e) => {
      e.preventDefault();
      setTalk(false, showBanner);
    };
    talkBtn.addEventListener('pointerdown', down);
    talkBtn.addEventListener('pointerup', up);
    talkBtn.addEventListener('pointercancel', up);
    talkBtn.addEventListener('pointerleave', up);
  });

  // 页面切后台时把流停掉，省带宽也省电。回来时网关会重发计划。
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
      stopAll();
      talkStop();
    } else if (media.plan) {
      renderMediaPanel(media.plan, showBanner);
    }
  });
}

// 能力上报只在启动时发过一次，那会儿 WebSocket 几乎肯定还没连上，消息被 send
// 丢掉，网关便一直按「只支持 H.264」下发计划。链路每次连上都补发一次。
function onLinkOpen() {
  if (sendRef) reportCaps(sendRef);
}

window.X30Media = { initMedia, onMediaPlan, stopAll, onLayout, setTalk, onLinkOpen };
