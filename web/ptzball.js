// 布控球：画中画模式 + 小摇杆云台。球机是 EVERET anv CGI，不是海康 ISAPI。
// App 走原生 HTTP（2.4G 绑射频，和拉 RTSP 同一张网卡）；网页走网关转发。

'use strict';

(function () {
  const PIP_MODES = [
    { id: 0, label: '默认' },
    { id: 1, label: '变焦主图' },
    { id: 2, label: '热像主图' },
    { id: 3, label: '左右拼接' },
    { id: 4, label: '仅变焦' },
    { id: 5, label: '仅热像' },
  ];
  const PIP_KEY = 'x30.ptz.pip.mode';
  // admin 的 MD5。现场账号密码都是 admin，CGI 只要 hex。
  const USER_MD5 = '21232f297a57a5a743894a0e4a801fc3';
  const PWD_MD5 = '21232f297a57a5a743894a0e4a801fc3';

  let pipMode = 2;
  let pipSize = 0;
  let pipPos = 0;
  let lastMoveKey = '';
  let busy = false;

  function host() {
    const u = (window.X30DogCam && window.X30DogCam.ptzUrl)
      ? window.X30DogCam.ptzUrl()
      : 'rtsp://192.168.1.168:554/11';
    const m = /rtsp:\/\/(?:[^/?#@]*@)?([^:/?#]+)/i.exec(String(u || ''));
    return (m && m[1]) || '192.168.1.168';
  }

  function radioBound() {
    return document.documentElement.classList.contains('radio-24');
  }

  function native() {
    const n = window.X30Native;
    return n && typeof n.cameraGetFire === 'function' ? n : null;
  }

  function cgiUrl(cgi, qs) {
    return 'http://' + host() + '/cgi-bin/anv/' + cgi +
      '?user=' + USER_MD5 + '&pwd=' + PWD_MD5 + '&' + qs +
      '&Cache=' + Math.random();
  }

  function fire(cgi, qs) {
    // MESH 上平板是 10 网，打不到 1 网的球；云台走网关 CGI。
    if (!radioBound()) return false;
    const n = native();
    if (n) {
      n.cameraGetFire(cgiUrl(cgi, qs), true);
      return true;
    }
    return false;
  }

  function wait(cgi, qs) {
    if (!radioBound()) return '';
    const n = native();
    if (n && typeof n.cameraGet === 'function') {
      return String(n.cameraGet(cgiUrl(cgi, qs), true) || '');
    }
    return '';
  }

  function parsePip(body) {
    if (!body || body.indexOf('mode=') === -1) return null;
    const kv = {};
    String(body).split(/\r?\n/).forEach((line) => {
      const i = line.indexOf('=');
      if (i > 0) kv[line.slice(0, i).trim()] = line.slice(i + 1).trim();
    });
    return {
      mode: parseInt(kv.mode, 10) || 0,
      size: parseInt(kv.SmallPicSize, 10) || 0,
      pos: parseInt(kv.SmallPicPos, 10) || 0,
    };
  }

  function rememberMode(mode) {
    pipMode = mode;
    try { window.localStorage.setItem(PIP_KEY, String(mode)); } catch (e) { /* */ }
  }

  function labelOf(mode) {
    const row = PIP_MODES.find((m) => m.id === mode);
    return row ? row.label : ('模式 ' + mode);
  }

  function paintPip() {
    const btn = document.getElementById('btn-ptz-pip');
    if (btn) btn.textContent = '画中画 · ' + labelOf(pipMode);
  }

  function applyPip(st) {
    if (!st) return;
    pipMode = st.mode;
    if (st.size != null) pipSize = st.size;
    if (st.pos != null) pipPos = st.pos;
    rememberMode(pipMode);
    paintPip();
  }

  function speedOf(pan, tilt, zoom) {
    const mag = Math.max(Math.abs(pan), Math.abs(tilt), Math.abs(zoom));
    return Math.max(1, Math.min(8, Math.round(mag * 8)));
  }

  function dirOf(pan, tilt) {
    const r = pan > 0.08 ? 1 : (pan < -0.08 ? -1 : 0);
    const u = tilt > 0.08 ? 1 : (tilt < -0.08 ? -1 : 0);
    if (!r && !u) return '';
    if (u > 0 && r < 0) return 'UpLeft';
    if (u > 0 && r > 0) return 'UpRight';
    if (u < 0 && r < 0) return 'DownLeft';
    if (u < 0 && r > 0) return 'DownRight';
    if (u > 0) return 'Up';
    if (u < 0) return 'Down';
    return r < 0 ? 'Left' : 'Right';
  }

  function move(pan, tilt, zoom) {
    const p = Math.abs(pan) > 0.08 ? pan : 0;
    const t = Math.abs(tilt) > 0.08 ? tilt : 0;
    const z = Math.abs(zoom) > 0.08 ? zoom : 0;
    const key = (p ? (p > 0 ? 'R' : 'L') : '') +
      (t ? (t > 0 ? 'U' : 'D') : '') +
      (z ? (z > 0 ? '+' : '-') : '');
    if (!key) {
      if (lastMoveKey) {
        lastMoveKey = '';
        fire('ptz_cgi', 'action=Stop&Speed=1');
      }
      return true;
    }
    if (key === lastMoveKey) return true;
    lastMoveKey = key;
    const speed = speedOf(p, t, z);
    const dir = dirOf(p, t);
    if (dir) fire('ptz_cgi', 'action=' + dir + '&Speed=' + speed);
    if (z) fire('ptz_cgi', 'action=' + (z > 0 ? 'ZoomAdd' : 'ZoomSub') + '&Speed=' + speed);
    return !!native();
  }

  function setPip(mode) {
    const next = ((mode % 6) + 6) % 6;
    const qs = 'action=set&mode=' + next +
      '&SmallPicSize=' + pipSize +
      '&SmallPicPos=' + pipPos +
      '&CustomSmallPicX=0&CustomSmallPicY=0';
    if (native()) {
      fire('pip_cgi', qs);
      rememberMode(next);
      paintPip();
      return true;
    }
    if (window.X30PtzBallSend) {
      window.X30PtzBallSend({ t: 'ptz_pip', mode: next, size: pipSize, pos: pipPos });
      rememberMode(next);
      paintPip();
      return true;
    }
    return false;
  }

  function refreshPip(send) {
    if (native()) {
      const st = parsePip(wait('pip_cgi', 'action=get'));
      if (st) applyPip(st);
      return;
    }
    if (send) send({ t: 'ptz_pip' });
  }

  function cyclePip(speak, banner) {
    if (busy) return;
    busy = true;
    setTimeout(() => { busy = false; }, 400);
    const next = (pipMode + 1) % PIP_MODES.length;
    if (!setPip(next)) {
      if (banner) banner('球机未接通，无法切画中画');
      return;
    }
    const name = labelOf(next);
    if (banner) banner('画中画：' + name);
    if (speak) speak(name);
  }

  function onPipMsg(msg) {
    if (!msg || msg.t !== 'ptz_pip') return false;
    if (msg.ok === false) return true;
    applyPip({
      mode: Number(msg.mode),
      size: msg.size != null ? Number(msg.size) : pipSize,
      pos: msg.pos != null ? Number(msg.pos) : pipPos,
    });
    return true;
  }

  function init(send, speak, banner) {
    try {
      const saved = parseInt(window.localStorage.getItem(PIP_KEY), 10);
      if (saved >= 0 && saved <= 5) pipMode = saved;
    } catch (e) { /* */ }
    paintPip();
    const btn = document.getElementById('btn-ptz-pip');
    if (btn) {
      btn.addEventListener('click', (ev) => {
        ev.stopPropagation();
        cyclePip(speak, banner);
      });
    }
    refreshPip(send);
  }

  window.X30PtzBall = {
    init, move, setPip, cyclePip, refreshPip, onPipMsg, paintPip,
  };
})();
