// 视频与对讲。协议见 docs/app-protocol.md，设计依据见 docs/media-architecture.md。
//
// 三件事：探测本机能解什么编码并上报、按网关下发的「媒体计划」拉流、按住说话。
// 视频不经过网关，直接从 MediaMTX 走 WebRTC 拉，网关只负责编排。

'use strict';

const media = {
  plan: null,
  main: null,          // 当前主视图的 source id
  pcs: new Map(),      // sourceId -> RTCPeerConnection
  talkPc: null,
  talkStream: null,
  caps: { h264: true, h265: false },
};

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
  } catch {
    return null;
  }
}

async function reportCaps(sendFn) {
  media.caps = probeCodecsSync();
  const confirmed = await confirmH265();
  if (confirmed !== null) media.caps.h265 = confirmed;

  sendFn({ t: 'media_caps', h264: media.caps.h264, h265: media.caps.h265 });

  const el = document.getElementById('codec-note');
  if (el) {
    el.textContent = media.caps.h265
      ? 'H.265 可用'
      : 'H.265 不可用，将使用 H.264（画质相同则占用约两倍带宽）';
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

function stopSource(id) {
  const pc = media.pcs.get(id);
  if (pc) {
    pc.close();
    media.pcs.delete(id);
  }
}

function stopAll() {
  for (const id of Array.from(media.pcs.keys())) stopSource(id);
}

// ---------------------------------------------------------------------------
// 媒体计划
// ---------------------------------------------------------------------------

function onMediaPlan(plan, showBanner) {
  media.plan = plan;
  media.main = plan.main || null;
  renderMediaPanel(plan, showBanner);
}

function renderMediaPanel(plan, showBanner) {
  const panel = document.getElementById('media-list');
  if (!panel) return;

  const wanted = new Set();
  panel.innerHTML = '';

  for (const s of plan.sources) {
    const row = document.createElement('div');
    row.className = 'media-row' + (s.id === plan.main ? ' is-main' : '');

    const btn = document.createElement('button');
    btn.className = 'btn media-pick';
    btn.textContent = s.name;
    btn.disabled = !s.available;
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

    if (s.available) wanted.add(s.id);
  }

  const budget = document.getElementById('media-budget');
  if (budget) {
    budget.textContent = `合计 ${plan.total_kbps} / ${plan.budget_kbps} kbps`;
    budget.classList.toggle('warn', !!plan.over_budget);
  }
  if (plan.over_budget && showBanner) {
    showBanner('视频码率超出链路预算，请降低相机码率设置', 9000);
  }

  // 只拉主视图这一路。缩略图同时拉三路会把链路吃掉，而且遥控端只有一块屏，
  // 缩略图的价值是「看一眼有没有情况」，做成点开才拉更合适。
  for (const id of Array.from(media.pcs.keys())) {
    if (id !== plan.main) stopSource(id);
  }
  const mainSrc = plan.sources.find((s) => s.id === plan.main && s.available);
  const videoEl = document.getElementById('media-video');
  if (mainSrc && videoEl && !media.pcs.has(mainSrc.id)) {
    whepPlay(plan.webrtc_base, mainSrc.path, videoEl)
      .then((pc) => media.pcs.set(mainSrc.id, pc))
      .catch((e) => {
        if (showBanner) showBanner(`拉流失败：${e.message}`, 6000);
      });
  }
  if (!mainSrc && videoEl) videoEl.srcObject = null;
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

function initMedia(sendFn, showBanner) {
  sendRef = sendFn;
  reportCaps(sendFn);

  const talkBtn = document.getElementById('btn-talk');
  if (talkBtn) {
    const down = (e) => { e.preventDefault(); talkBtn.classList.add('active'); talkStart(showBanner); };
    const up = (e) => { e.preventDefault(); talkBtn.classList.remove('active'); talkStop(); };
    talkBtn.addEventListener('pointerdown', down);
    talkBtn.addEventListener('pointerup', up);
    talkBtn.addEventListener('pointercancel', up);
    talkBtn.addEventListener('pointerleave', up);
  }

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

window.X30Media = { initMedia, onMediaPlan, stopAll };
