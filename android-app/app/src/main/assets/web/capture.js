// 主画面截图 / 录屏。对讲仍走 media.js 的按住说话。
//
// 截图把当前这一格画到一张图里下载。录屏用 MediaRecorder 吃这一格的画面
// （视频走元素上的 captureStream，点云走 canvas.captureStream）。
// 浏览器拦下载时，文件会进系统默认下载目录；WebView 有的版本会改成打开预览。

'use strict';

const VIDEO_OF = {
  dog_cam: 'media-video',
  ptz_vis: 'media-video-ptz-vis',
  ptz_ir: 'media-video-ptz-ir',
};

const NAME_OF = {
  dog_cam: '机身相机',
  ptz_vis: '布控球白光',
  ptz_ir: '布控球热成像',
  cloud: '点云',
};

const recorders = new Map(); // view -> { rec, chunks, btn }

// 语音见 voice.js。按下去时它已经按按钮上的字念过「截图」了，这里只念结果不同的
// 那几种：没画面截不了、录屏落盘了。G20 的截图键更需要 —— 那条路上没有按钮，
// 人也不在看屏幕，不念就完全不知道有没有存下来。
// 名字带 cap 前缀：几个 web/*.js 共用一个全局作用域，顶层重名会让后加载的整份挂掉。
function capSay(text) {
  if (window.X30Voice) window.X30Voice.say(text);
}

function stamp() {
  const d = new Date();
  const p = (n) => (n < 10 ? '0' : '') + n;
  return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-` +
         `${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`;
}

function saveBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 4000);
}

function videoEl(view) {
  const id = VIDEO_OF[view];
  return id ? document.getElementById(id) : null;
}

function shotVideo(view, showBanner) {
  const video = videoEl(view);
  if (!video || video.videoWidth < 2) {
    showBanner((NAME_OF[view] || view) + '还没有画面，截不了图');
    capSay('没有画面');
    return;
  }
  const c = document.createElement('canvas');
  c.width = video.videoWidth;
  c.height = video.videoHeight;
  c.getContext('2d').drawImage(video, 0, 0);
  c.toBlob((blob) => {
    if (!blob) {
      showBanner('截图失败');
      capSay('截图失败');
      return;
    }
    saveBlob(blob, `x30-${view}-${stamp()}.png`);
    showBanner('已截图 ' + (NAME_OF[view] || view), 2500);
    capSay('已截图');
  }, 'image/png');
}

function shotCloud(showBanner) {
  const canvas = document.getElementById('cloud-canvas');
  if (!canvas) return;
  try {
    canvas.toBlob((blob) => {
      if (!blob) {
        showBanner('点云截图失败');
        capSay('截图失败');
        return;
      }
      saveBlob(blob, `x30-cloud-${stamp()}.png`);
      showBanner('已截图 点云', 2500);
      capSay('已截图');
    }, 'image/png');
  } catch (e) {
    showBanner('点云截图失败：' + e.message);
    capSay('截图失败');
  }
}

function pickMime() {
  if (typeof MediaRecorder === 'undefined') return '';
  const cands = [
    'video/webm;codecs=vp8',
    'video/webm',
    'video/mp4',
  ];
  for (const t of cands) {
    if (MediaRecorder.isTypeSupported && MediaRecorder.isTypeSupported(t)) return t;
  }
  return '';
}

function streamOf(view) {
  if (view === 'cloud') {
    const canvas = document.getElementById('cloud-canvas');
    if (!canvas || !canvas.captureStream) return null;
    return canvas.captureStream(12);
  }
  const video = videoEl(view);
  if (!video) return null;
  if (video.captureStream) return video.captureStream();
  if (video.mozCaptureStream) return video.mozCaptureStream();
  if (video.srcObject) return video.srcObject;
  return null;
}

function stopRec(view) {
  const slot = recorders.get(view);
  if (!slot) return;
  try { if (slot.rec.state !== 'inactive') slot.rec.stop(); } catch (e) { /* 已经停了 */ }
}

function startRec(view, btn, showBanner) {
  if (typeof MediaRecorder === 'undefined') {
    showBanner('当前浏览器不支持录屏');
    return;
  }
  const stream = streamOf(view);
  if (!stream || stream.getVideoTracks().length === 0) {
    showBanner((NAME_OF[view] || view) + '还没有画面，录不了');
    capSay('没有画面');
    return;
  }
  const mime = pickMime();
  let rec;
  try {
    rec = mime ? new MediaRecorder(stream, { mimeType: mime }) : new MediaRecorder(stream);
  } catch (e) {
    showBanner('录屏启动失败：' + e.message);
    return;
  }
  const chunks = [];
  rec.ondataavailable = (ev) => { if (ev.data && ev.data.size) chunks.push(ev.data); };
  rec.onstop = () => {
    recorders.delete(view);
    btn.classList.remove('active');
    btn.textContent = '录屏';
    const type = rec.mimeType || mime || 'video/webm';
    const ext = type.indexOf('mp4') >= 0 ? 'mp4' : 'webm';
    if (!chunks.length) {
      showBanner('录屏没有写到数据');
      capSay('录屏没有数据');
      return;
    }
    saveBlob(new Blob(chunks, { type }), `x30-${view}-${stamp()}.${ext}`);
    showBanner('录屏已保存', 2500);
    capSay('录屏已保存');
  };
  rec.onerror = () => {
    recorders.delete(view);
    btn.classList.remove('active');
    btn.textContent = '录屏';
    showBanner('录屏出错');
    capSay('录屏出错');
  };
  recorders.set(view, { rec, btn });
  rec.start(1000);
  btn.classList.add('active');
  btn.textContent = '停止';
  showBanner('开始录屏 ' + (NAME_OF[view] || view), 2000);
  capSay('开始录屏');
}

function toggleRec(view, btn, showBanner) {
  if (recorders.has(view)) {
    stopRec(view);
    return;
  }
  startRec(view, btn, showBanner);
}

function currentView() {
  const main = document.querySelector('.pane.is-main');
  return (main && main.dataset.view) || 'dog_cam';
}

function dummyRecBtn() {
  return { classList: { add: function () {}, remove: function () {} }, textContent: '' };
}

function recBtnOf(view) {
  const pane = document.querySelector('.pane[data-view="' + view + '"]');
  return (pane && pane.querySelector('[data-capture="rec"]')) || dummyRecBtn();
}

function shotCurrent(showBanner) {
  const view = currentView();
  if (view === 'cloud') shotCloud(showBanner);
  else shotVideo(view, showBanner);
}

function toggleRecCurrent(showBanner) {
  const view = currentView();
  toggleRec(view, recBtnOf(view), showBanner);
}

function initCapture(showBanner) {
  document.querySelectorAll('[data-capture]').forEach((btn) => {
    btn.addEventListener('click', (e) => {
      e.preventDefault();
      e.stopPropagation();
      const pane = btn.closest('.pane');
      const view = pane && pane.dataset.view;
      if (!view) return;
      if (btn.dataset.capture === 'shot') {
        if (view === 'cloud') shotCloud(showBanner);
        else shotVideo(view, showBanner);
      } else {
        toggleRec(view, btn, showBanner);
      }
    });
  });

  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) return;
    for (const view of Array.from(recorders.keys())) stopRec(view);
  });
}

window.X30Capture = { initCapture, shotCurrent, toggleRecCurrent };
