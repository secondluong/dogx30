// 点云渲染。WebGL 直接画，不引 three.js。
//
// LIO 就绪后网关下发世界系 /cloud_registered（flags bit0）。轨迹和点
// 都在同一套 LIO 坐标里，显示时再变到当前机体系。LIO 未就绪时仍是
// 机体系 /lidar_points，用扫描定位位姿把历史帧拼起来。
//
// 坐标还原：真实坐标 = origin + uint16 × scale。

(function () {
  'use strict';

  const MAGIC = 0x43303358; // "X30C" 小端读成 u32
  const HEADER_SIZE = 40;
  const MAX_LIVE_FRAMES = 80;
  const MAX_PERSIST = 160000;
  const MAX_TRAIL = 2000;

  let gl = null;
  let program = null;
  let lineProgram = null;
  let buffer = null;
  let lineBuffer = null;
  let canvas = null;
  let pointCount = 0;
  let subscribed = false;
  let wanted = false;
  let sendFn = null;
  let lastStatus = null;

  const pose = { x: 0, y: 0, yaw: 0 };
  const frames = [];
  const trail = [];
  // 单兵 UWB，单位米，相对狗。现场 XY 与机体系差 90°，
  // 顺时针转到点云：(x, y) → (y, -x)。
  let soldiers = [];
  const persistMap = new Map();
  const est = {
    x: 0, y: 0, yaw: 0, t: 0, mile: null,
    odomLive: false,
    lastOdomX: null, lastOdomY: null, lastOdomYaw: 0, lastOdomT: 0,
    lastIngestX: null, lastIngestY: null,
    source: '',
    world: false,
  };

  const opts = {
    persist: false,
    accumMs: 30000,   // 实时：固定累积最近 30 秒（设置里不再提供单帧/3s/10s）
    voxel: 0.10,      // 固定 10 cm 体素
    trail: true,
    showPoints: true,
    slice: true,
    sliceZ: 0,
    sliceHalf: 2.0,
    storyH: 3.0,
    displayH: 2.0,   // 层高显示：从地面往上留多少米，不跟楼层切割开关走
    floorCut: false,
    floorView: -1,
    floors: [],
  };

  // 相机：绕原点的轨道视角。仰角限制在两极之间，避免翻转。
  const cam = { yaw: -2.4, pitch: 0.95, dist: 18, lookX: 0, lookY: 0, lookZ: 0 };
  let dragging = false;
  let lastX = 0;
  let lastY = 0;

  const VERT = `
    attribute vec3 a_p;
    uniform mat4 u_mvp;
    uniform float u_size;
    varying float v_h;
    void main() {
      v_h = a_p.z;
      gl_Position = u_mvp * vec4(a_p, 1.0);
      gl_PointSize = clamp(u_size / max(gl_Position.w, 0.8), 3.0, 14.0);
    }
  `;

  // 按层高窗口着色：墙脚青、墙身黄。点画大一点互相盖住，墙看起来才实。
  const FRAG = `
    precision mediump float;
    varying float v_h;
    uniform float u_slice;
    uniform float u_z0;
    uniform float u_zHalf;
    void main() {
      if (u_slice > 0.5 && (v_h < u_z0 || v_h > u_z0 + u_zHalf)) discard;
      vec2 pc = gl_PointCoord * 2.0 - 1.0;
      if (dot(pc, pc) > 1.05) discard;
      float t = u_zHalf > 0.05
        ? clamp((v_h - u_z0) / u_zHalf, 0.0, 1.0)
        : clamp((v_h + 0.4) / 2.4, 0.0, 1.0);
      vec3 low  = vec3(0.22, 0.84, 0.90);
      vec3 mid  = vec3(0.78, 0.94, 0.28);
      vec3 high = vec3(0.99, 0.92, 0.18);
      vec3 c = t < 0.38 ? mix(low, mid, t / 0.38) : mix(mid, high, (t - 0.38) / 0.62);
      gl_FragColor = vec4(c, 1.0);
    }
  `;

  const LINE_VERT = `
    attribute vec3 a_p;
    uniform mat4 u_mvp;
    uniform float u_size;
    void main() {
      gl_Position = u_mvp * vec4(a_p, 1.0);
      gl_PointSize = u_size;
    }
  `;
  const LINE_FRAG = `
    precision mediump float;
    uniform vec3 u_color;
    void main() { gl_FragColor = vec4(u_color, 1.0); }
  `;

  function compile(type, src) {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
      console.error('着色器编译失败:', gl.getShaderInfoLog(s));
      return null;
    }
    return s;
  }

  function link(vsSrc, fsSrc) {
    const vs = compile(gl.VERTEX_SHADER, vsSrc);
    const fs = compile(gl.FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) return null;
    const p = gl.createProgram();
    gl.attachShader(p, vs);
    gl.attachShader(p, fs);
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
      console.error('着色器链接失败:', gl.getProgramInfoLog(p));
      return null;
    }
    return p;
  }

  function initGl() {
    canvas = document.getElementById('cloud-canvas');
    if (!canvas) return false;
    // preserveDrawingBuffer：截图/录屏要从画布读像素。默认会在提交后清掉。
    gl = canvas.getContext('webgl', {
      antialias: false, alpha: false, preserveDrawingBuffer: true,
    });
    if (!gl) {
      const note = document.getElementById('cloud-note');
      if (note) note.textContent = '此设备不支持 WebGL，点云无法显示';
      return false;
    }

    program = link(VERT, FRAG);
    lineProgram = link(LINE_VERT, LINE_FRAG);
    if (!program || !lineProgram) return false;

    buffer = gl.createBuffer();
    lineBuffer = gl.createBuffer();
    gl.clearColor(0.02, 0.02, 0.04, 1.0);
    gl.enable(gl.DEPTH_TEST);

    bindCamera();
    resize();
    window.addEventListener('resize', resize);
    return true;
  }

  function resize() {
    if (!canvas) return;
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.floor(canvas.clientWidth * dpr));
    const h = Math.max(1, Math.floor(canvas.clientHeight * dpr));
    if (canvas.width !== w || canvas.height !== h) {
      canvas.width = w;
      canvas.height = h;
    }
    draw();
  }

  function bindCamera() {
    canvas.addEventListener('pointerdown', (e) => {
      dragging = true;
      lastX = e.clientX;
      lastY = e.clientY;
      canvas.setPointerCapture(e.pointerId);
    });
    canvas.addEventListener('pointermove', (e) => {
      if (!dragging) return;
      cam.yaw += (e.clientX - lastX) * 0.008;
      cam.pitch += (e.clientY - lastY) * 0.008;
      cam.pitch = Math.max(-1.5, Math.min(1.5, cam.pitch));
      lastX = e.clientX;
      lastY = e.clientY;
      draw();
    });
    const stop = (e) => {
      dragging = false;
      if (e.pointerId !== undefined && canvas.hasPointerCapture(e.pointerId)) {
        canvas.releasePointerCapture(e.pointerId);
      }
    };
    canvas.addEventListener('pointerup', stop);
    canvas.addEventListener('pointercancel', stop);
    canvas.addEventListener('wheel', (e) => {
      e.preventDefault();
      cam.dist *= e.deltaY > 0 ? 1.12 : 0.89;
      cam.dist = Math.max(2, Math.min(60, cam.dist));
      draw();
    }, { passive: false });
  }

  // 手搓 MVP，省掉矩阵库。列主序，直接喂给 uniformMatrix4fv。
  function mvpMatrix(aspect) {
    const cp = Math.cos(cam.pitch), sp = Math.sin(cam.pitch);
    const cy = Math.cos(cam.yaw), sy = Math.sin(cam.yaw);

    const eye = [
      cam.lookX + cam.dist * cp * cy,
      cam.lookY + cam.dist * cp * sy,
      cam.lookZ + cam.dist * sp + 1.0,
    ];
    const target = [cam.lookX, cam.lookY, cam.lookZ];
    const up = [0, 0, 1];

    const f = norm(sub(target, eye));
    const s = norm(cross(f, up));
    const u = cross(s, f);

    const view = [
      s[0], u[0], -f[0], 0,
      s[1], u[1], -f[1], 0,
      s[2], u[2], -f[2], 0,
      -dot(s, eye), -dot(u, eye), dot(f, eye), 1,
    ];

    const fov = 55 * Math.PI / 180;
    const nf = 1 / (0.2 - 200);
    const t = 1 / Math.tan(fov / 2);
    const proj = [
      t / aspect, 0, 0, 0,
      0, t, 0, 0,
      0, 0, (200 + 0.2) * nf, -1,
      0, 0, 2 * 200 * 0.2 * nf, 0,
    ];

    return mul(proj, view);
  }

  function sub(a, b) { return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]; }
  function dot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
  function cross(a, b) {
    return [a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]];
  }
  function norm(a) {
    const l = Math.hypot(a[0], a[1], a[2]) || 1;
    return [a[0] / l, a[1] / l, a[2] / l];
  }
  function mul(a, b) {
    const o = new Float32Array(16);
    for (let c = 0; c < 4; c++) {
      for (let r = 0; r < 4; r++) {
        o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
                       a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
      }
    }
    return o;
  }

  function bodyToOdom(x, y, z, p) {
    const c = Math.cos(p.yaw), s = Math.sin(p.yaw);
    return [p.x + c * x - s * y, p.y + s * x + c * y, z];
  }

  function odomToBody(x, y, z, p) {
    const dx = x - p.x, dy = y - p.y;
    const c = Math.cos(p.yaw), s = Math.sin(p.yaw);
    return [c * dx + s * dy, -s * dx + c * dy, z];
  }

  function voxelKey(x, y, z, inv) {
    const ix = Math.floor(x * inv);
    const iy = Math.floor(y * inv);
    const iz = Math.floor(z * inv);
    return ix * 73856093 + iy * 19349663 + iz * 83492791;
  }

  function voxelize(src, n, voxel) {
    if (n === 0) return { xyz: src, count: 0 };
    if (voxel <= 1e-4) return { xyz: src, count: n };
    const inv = 1 / voxel;
    const seen = new Map();
    for (let i = 0; i < n; i++) {
      const x = src[i * 3], y = src[i * 3 + 1], z = src[i * 3 + 2];
      const key = voxelKey(x, y, z, inv);
      if (!seen.has(key)) seen.set(key, i);
    }
    const count = seen.size;
    const xyz = new Float32Array(count * 3);
    let k = 0;
    seen.forEach((i) => {
      xyz[k++] = src[i * 3];
      xyz[k++] = src[i * 3 + 1];
      xyz[k++] = src[i * 3 + 2];
    });
    return { xyz, count };
  }

  function decodeFrame(view, origin, scale, count) {
    const xyz = new Float32Array(count * 3);
    let o = HEADER_SIZE;
    for (let i = 0; i < count; i++) {
      xyz[i * 3] = origin[0] + view.getUint16(o, true) * scale;
      xyz[i * 3 + 1] = origin[1] + view.getUint16(o + 2, true) * scale;
      xyz[i * 3 + 2] = origin[2] + view.getUint16(o + 4, true) * scale;
      o += 6;
    }
    return xyz;
  }

  function ingestPersist(xyz, n, from, world) {
    const inv = 1 / Math.max(opts.voxel, 0.05);
    for (let i = 0; i < n && persistMap.size < MAX_PERSIST; i++) {
      const w = world
        ? [xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2]]
        : bodyToOdom(xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2], from);
      const key = voxelKey(w[0], w[1], w[2], inv);
      if (!persistMap.has(key)) persistMap.set(key, w);
    }
  }

  function rebuild() {
    const now = pose;
    let src;
    let n = 0;

    if (opts.persist && persistMap.size > 0) {
      src = new Float32Array(persistMap.size * 3);
      persistMap.forEach((w) => {
        const b = odomToBody(w[0], w[1], w[2], now);
        src[n++] = b[0];
        src[n++] = b[1];
        src[n++] = b[2];
      });
      n /= 3;
    } else {
      const cutoff = opts.accumMs > 0 ? Date.now() - opts.accumMs : 0;
      const use = [];
      for (let i = 0; i < frames.length; i++) {
        const f = frames[i];
        if (opts.accumMs === 0) {
          if (i === frames.length - 1) use.push(f);
        } else if (f.t >= cutoff) {
          use.push(f);
        }
      }
      let total = 0;
      for (let i = 0; i < use.length; i++) total += use[i].count;
      src = new Float32Array(total * 3);
      for (let i = 0; i < use.length; i++) {
        const f = use[i];
        const samePose = !f.world &&
                         Math.abs(f.pose.x - now.x) < 1e-4 &&
                         Math.abs(f.pose.y - now.y) < 1e-4 &&
                         Math.abs(f.pose.yaw - now.yaw) < 1e-4;
        for (let k = 0; k < f.count; k++) {
          const x = f.xyz[k * 3], y = f.xyz[k * 3 + 1], z = f.xyz[k * 3 + 2];
          if (f.world) {
            const b = odomToBody(x, y, z, now);
            src[n++] = b[0]; src[n++] = b[1]; src[n++] = b[2];
          } else if (samePose) {
            src[n++] = x; src[n++] = y; src[n++] = z;
          } else {
            const w = bodyToOdom(x, y, z, f.pose);
            const b = odomToBody(w[0], w[1], w[2], now);
            src[n++] = b[0]; src[n++] = b[1]; src[n++] = b[2];
          }
        }
      }
      n /= 3;
    }

    const packed = voxelize(src, n, opts.voxel);
    if (!gl) {
      pointCount = packed.count;
      return;
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, packed.xyz, gl.DYNAMIC_DRAW);
    pointCount = packed.count;
  }

  function trailPtsBody() {
    let start = 0;
    for (let i = 1; i < trail.length; i++) {
      if (Math.hypot(trail[i].x - trail[i - 1].x, trail[i].y - trail[i - 1].y) > 2.5) {
        start = i;
      }
    }
    const pts = [];
    for (let i = start; i < trail.length; i++) {
      pts.push(odomToBody(trail[i].x, trail[i].y, 0.22, pose));
    }
    return pts;
  }

  // WebGL 线宽几乎总是 1px，埋在点云里看不见。轨迹改成地面上的一条带子。
  function trailRibbonVerts() {
    const pts = trailPtsBody();
    const half = 0.045;
    const out = [];
    for (let i = 0; i < pts.length; i++) {
      const prev = pts[Math.max(0, i - 1)];
      const next = pts[Math.min(pts.length - 1, i + 1)];
      let dx = next[0] - prev[0], dy = next[1] - prev[1];
      const len = Math.hypot(dx, dy);
      if (len < 1e-4) { dx = 1; dy = 0; }
      else { dx /= len; dy /= len; }
      const nx = -dy * half, ny = dx * half;
      const p = pts[i];
      out.push(p[0] + nx, p[1] + ny, p[2], p[0] - nx, p[1] - ny, p[2]);
    }
    return new Float32Array(out);
  }

  function markerVerts() {
    const z = 0.28;
    const out = [0, 0, z];
    const n = 12;
    for (let i = 0; i <= n; i++) {
      const a = (i / n) * Math.PI * 2;
      out.push(Math.cos(a) * 0.20, Math.sin(a) * 0.20, z);
    }
    return new Float32Array(out);
  }

  function collectZs() {
    const zs = [];
    if (opts.persist && persistMap.size > 0) {
      persistMap.forEach((w) => { zs.push(w[2]); });
    } else {
      for (let i = 0; i < frames.length; i++) {
        const f = frames[i];
        for (let k = 0; k < f.count; k++) zs.push(f.xyz[k * 3 + 2]);
      }
    }
    if (zs.length <= 5000) return zs;
    const stride = Math.ceil(zs.length / 4000);
    const out = [];
    for (let i = 0; i < zs.length; i += stride) out.push(zs[i]);
    return out;
  }

  // 按层高把点云切成若干层。一层里点数太少就丢掉（没走到的楼层不占档）。
  let lastZMin = 0;
  let zMinAt = 0;

  function heightBase() {
    const now = Date.now();
    if (now - zMinAt < 1500 && lastZMin !== 0) return lastZMin;
    const zs = collectZs();
    if (zs.length) {
      zs.sort((a, b) => a - b);
      lastZMin = zs[Math.floor(zs.length * 0.03)];
      zMinAt = now;
    }
    return lastZMin;
  }

  function detectFloors() {
    const story = Math.max(opts.storyH, 1.2);
    const zs = collectZs();
    if (!zs.length) {
      opts.floors = [0];
      return opts.floors;
    }
    zs.sort((a, b) => a - b);
    const zMin = zs[Math.floor(zs.length * 0.03)];
    const zMax = zs[Math.floor(zs.length * 0.97)];
    lastZMin = zMin;
    zMinAt = Date.now();
    const minCount = Math.max(20, zs.length * 0.015);
    const floors = [];
    const start = zMin;
    for (let z = start; z <= zMax + 0.15; z += story) {
      let c = 0;
      const lo = z - 0.25;
      const hi = z + story - 0.15;
      for (let i = 0; i < zs.length; i++) {
        if (zs[i] >= lo && zs[i] < hi) c++;
      }
      if (c >= minCount) floors.push(Number(z.toFixed(2)));
    }
    opts.floors = floors.length ? floors : [Number(zMin.toFixed(2))];
    if (opts.floorView >= opts.floors.length) opts.floorView = -1;
    return opts.floors;
  }

  // 层高显示始终生效：从地面（或当前选的楼层）往上留 displayH 米。
  // 楼层切割只改「地面」取哪一层，不决定切不切。
  function applyFloorView() {
    opts.slice = true;
    if (opts.floorCut && opts.floorView >= 0 && opts.floors.length) {
      opts.sliceZ = opts.floors[opts.floorView];
    } else {
      opts.sliceZ = heightBase();
    }
    opts.sliceHalf = Math.max(0.2, opts.displayH);
  }

  function floorButtonText() {
    if (!opts.floorCut) return '楼层';
    if (opts.floorView < 0) return '全部';
    return (opts.floorView + 1) + 'F';
  }

  function headingVerts() {
    const z = 0.30;
    return new Float32Array([
      0.12, 0.08, z,  0.38, 0, z,  0.12, -0.08, z,
    ]);
  }

  function soldierRing(x, y, z) {
    const out = [x, y, z + 0.04];
    const n = 12;
    const r = 0.28;
    for (let i = 0; i <= n; i++) {
      const a = (i / n) * Math.PI * 2;
      out.push(x + Math.cos(a) * r, y + Math.sin(a) * r, z + 0.04);
    }
    return new Float32Array(out);
  }

  function paintUwbHud(uwb) {
    const el = document.getElementById('uwb-hud');
    if (!el) return;
    const tags = uwb && uwb.tags ? uwb.tags : [];
    if (!uwb || !uwb.heard || !tags.length) {
      el.classList.add('hidden');
      el.innerHTML = '';
      return;
    }
    let html = '';
    for (let i = 0; i < tags.length; i++) {
      const t = tags[i];
      const id = t.id;
      const stale = !t.valid;
      const xyz = t.valid
        ? (function () {
            const xy = uwbToCloud(Number(t.x), Number(t.y));
            return xy.x.toFixed(2) + '  ' + xy.y.toFixed(2) + '  ' +
                   Number(t.z).toFixed(2) + ' m';
          }())
        : '无效';
      html += '<div class="uwb-card' + (stale ? ' stale' : '') +
              '" data-id="' + id + '">' +
              '<div class="uwb-top"><span><span class="uwb-dot"></span>单兵 ' +
              id + '</span><span>相对狗</span></div>' +
              '<div class="uwb-xyz">' + xyz + '</div></div>';
    }
    el.innerHTML = html;
    el.classList.remove('hidden');
  }

  function projectSoldier(mvp, x, y, z) {
    const cx = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
    const cy = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
    const cw = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
    if (cw <= 0.08) return null;
    const ndcX = cx / cw;
    const ndcY = cy / cw;
    if (ndcX < -1.15 || ndcX > 1.15 || ndcY < -1.15 || ndcY > 1.15) return null;
    return {
      x: (ndcX * 0.5 + 0.5) * canvas.clientWidth,
      y: (-ndcY * 0.5 + 0.5) * canvas.clientHeight,
    };
  }

  function syncSoldierMarks() {
    const host = document.getElementById('uwb-marks');
    if (!host) return;
    while (host.children.length > soldiers.length) host.removeChild(host.lastChild);
    while (host.children.length < soldiers.length) {
      const el = document.createElement('div');
      el.className = 'uwb-mark';
      el.innerHTML = '<span class="uwb-mark-body"><i class="uwb-mark-ico"></i>' +
                     '<b class="uwb-mark-id"></b></span><i class="uwb-mark-stem"></i>';
      host.appendChild(el);
    }
    for (let i = 0; i < soldiers.length; i++) {
      const el = host.children[i];
      const id = String(soldiers[i].id);
      el.dataset.id = id;
      el.querySelector('.uwb-mark-id').textContent = id;
    }
  }

  function placeSoldierMarks(mvp) {
    const host = document.getElementById('uwb-marks');
    if (!host || !canvas) return;
    for (let i = 0; i < soldiers.length; i++) {
      const el = host.children[i];
      if (!el) continue;
      const s = soldiers[i];
      const p = projectSoldier(mvp, s.x, s.y, s.z);
      if (!p) {
        el.classList.add('hidden');
        continue;
      }
      el.classList.remove('hidden');
      el.style.transform = 'translate(' + p.x.toFixed(1) + 'px,' +
                           p.y.toFixed(1) + 'px) translate(-50%,-100%)';
    }
  }

  function uwbToCloud(x, y) {
    return { x: y, y: -x };
  }

  function setSoldiers(uwb) {
    const next = [];
    const tags = uwb && uwb.tags ? uwb.tags : [];
    for (let i = 0; i < tags.length; i++) {
      const t = tags[i];
      if (!t || !t.valid) continue;
      const id = Number(t.id);
      const rawX = Number(t.x);
      const rawY = Number(t.y);
      const z = Number(t.z);
      if (!isFinite(id) || !isFinite(rawX) || !isFinite(rawY) || !isFinite(z)) continue;
      const xy = uwbToCloud(rawX, rawY);
      next.push({ id: id, x: xy.x, y: xy.y, z: z });
    }
    soldiers = next;
    paintUwbHud(uwb);
    syncSoldierMarks();
    if (gl) draw();
  }

  function draw() {
    if (!gl || !program) return;
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    const mvp = mvpMatrix(canvas.width / canvas.height);

    if (opts.showPoints && pointCount > 0) {
      gl.useProgram(program);
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
      const loc = gl.getAttribLocation(program, 'a_p');
      gl.enableVertexAttribArray(loc);
      gl.vertexAttribPointer(loc, 3, gl.FLOAT, false, 0, 0);
      gl.uniform1f(gl.getUniformLocation(program, 'u_size'), canvas.height / 70);
      gl.uniformMatrix4fv(gl.getUniformLocation(program, 'u_mvp'), false, mvp);
      gl.uniform1f(gl.getUniformLocation(program, 'u_slice'), opts.slice ? 1 : 0);
      gl.uniform1f(gl.getUniformLocation(program, 'u_z0'), opts.sliceZ);
      gl.uniform1f(gl.getUniformLocation(program, 'u_zHalf'), opts.displayH);
      gl.drawArrays(gl.POINTS, 0, pointCount);
    }

    if (!opts.trail && !soldiers.length) {
      placeSoldierMarks(mvp);
      return;
    }

    gl.disable(gl.DEPTH_TEST);
    gl.useProgram(lineProgram);
    const loc = gl.getAttribLocation(lineProgram, 'a_p');
    gl.enableVertexAttribArray(loc);
    gl.uniformMatrix4fv(gl.getUniformLocation(lineProgram, 'u_mvp'), false, mvp);
    gl.uniform1f(gl.getUniformLocation(lineProgram, 'u_size'), 10.0);

    if (opts.trail) {
      const ribbon = trailRibbonVerts();
      const nRibbon = ribbon.length / 3;
      if (nRibbon >= 4) {
        gl.uniform3f(gl.getUniformLocation(lineProgram, 'u_color'), 1.0, 0.12, 0.12);
        gl.bindBuffer(gl.ARRAY_BUFFER, lineBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, ribbon, gl.DYNAMIC_DRAW);
        gl.vertexAttribPointer(loc, 3, gl.FLOAT, false, 0, 0);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, nRibbon);
      }
    }

    const mark = markerVerts();
    gl.uniform3f(gl.getUniformLocation(lineProgram, 'u_color'), 1.0, 0.92, 0.15);
    gl.bindBuffer(gl.ARRAY_BUFFER, lineBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, mark, gl.DYNAMIC_DRAW);
    gl.vertexAttribPointer(loc, 3, gl.FLOAT, false, 0, 0);
    gl.drawArrays(gl.TRIANGLE_FAN, 0, mark.length / 3);

    const head = headingVerts();
    gl.uniform3f(gl.getUniformLocation(lineProgram, 'u_color'), 1.0, 0.25, 0.10);
    gl.bindBuffer(gl.ARRAY_BUFFER, lineBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, head, gl.DYNAMIC_DRAW);
    gl.vertexAttribPointer(loc, 3, gl.FLOAT, false, 0, 0);
    gl.drawArrays(gl.TRIANGLES, 0, 3);

    for (let i = 0; i < soldiers.length; i++) {
      const s = soldiers[i];
      if (s.id === 11) {
        gl.uniform3f(gl.getUniformLocation(lineProgram, 'u_color'), 0.25, 0.83, 1.0);
      } else {
        gl.uniform3f(gl.getUniformLocation(lineProgram, 'u_color'), 1.0, 0.48, 0.16);
      }
      const ring = soldierRing(s.x, s.y, s.z);
      gl.bindBuffer(gl.ARRAY_BUFFER, lineBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, ring, gl.DYNAMIC_DRAW);
      gl.vertexAttribPointer(loc, 3, gl.FLOAT, false, 0, 0);
      gl.drawArrays(gl.TRIANGLE_FAN, 0, ring.length / 3);
    }
    gl.enable(gl.DEPTH_TEST);
    placeSoldierMarks(mvp);
  }

  function onCloudFrame(arrayBuffer) {
    const view = new DataView(arrayBuffer);
    if (arrayBuffer.byteLength < HEADER_SIZE) return;
    if (view.getUint32(0, true) !== MAGIC) return;
    if (view.getUint8(4) !== 1) return;

    const origin = [view.getFloat32(20, true), view.getFloat32(24, true),
                    view.getFloat32(28, true)];
    const scale = view.getFloat32(32, true);
    const count = view.getUint32(36, true);
    if (HEADER_SIZE + count * 6 > arrayBuffer.byteLength) return;

    const xyz = decodeFrame(view, origin, scale, count);
    const flags = view.getUint8(5);
    const world = (flags & 1) !== 0;
    // 配准云已经在下时，间隙里漏出的机体帧必须丢掉。
    // 以前这里会清掉 persistMap，走过的地方看起来像没留下。
    if (est.world && !world) return;
    if (world !== est.world) {
      persistMap.clear();
      frames.length = 0;
      est.lastIngestX = null;
      est.lastIngestY = null;
      est.world = world;
    }
    const payloadEnd = HEADER_SIZE + count * 6;
    if (world && arrayBuffer.byteLength >= payloadEnd + 12) {
      const rx = view.getFloat32(payloadEnd, true);
      const ry = view.getFloat32(payloadEnd + 4, true);
      const ryaw = view.getFloat32(payloadEnd + 8, true);
      if (Number.isFinite(rx) && Number.isFinite(ry) && Number.isFinite(ryaw)) {
        pose.x = rx;
        pose.y = ry;
        pose.yaw = ryaw;
        est.x = rx;
        est.y = ry;
        est.yaw = ryaw;
        est.source = 'lio';
      }
    }
    const stamped = {
      t: Date.now(),
      pose: { x: pose.x, y: pose.y, yaw: pose.yaw },
      xyz: xyz,
      count: count,
      world: world,
    };
    frames.push(stamped);
    if (frames.length > MAX_LIVE_FRAMES) frames.splice(0, frames.length - MAX_LIVE_FRAMES);
    // 持久图按体素去重，每帧都叠。不能等「位姿移动 4cm」：
    // 转身扫到的墙、LIO 位姿偶发不动，都会让走过的地方丢了。
    if (opts.persist) ingestPersist(xyz, count, stamped.pose, world);

    // 帧到了就是订上了。只信本地按钮的话，重连或 2×2 切布局时角标会停在「未订阅」。
    if (!subscribed) {
      subscribed = true;
      syncSubscribeButton();
    }
    const idle = document.getElementById('cloud-idle');
    if (idle) idle.classList.add('hidden');
    if (opts.floorCut) detectFloors();
    applyFloorView();
    rebuild();
    draw();
    paintTag(lastStatus);
    const floorBtn = document.getElementById('btn-floor');
    if (floorBtn) floorBtn.textContent = floorButtonText();
  }

  function applyBodyDelta(dx, dy, yaw) {
    const c = Math.cos(yaw), s = Math.sin(yaw);
    est.x += c * dx - s * dy;
    est.y += s * dx + c * dy;
    pose.x = est.x;
    pose.y = est.y;
    pose.yaw = est.yaw;
  }

  function recordTrail(now) {
    const last = trail[trail.length - 1];
    const moved = !last || Math.hypot(est.x - last.x, est.y - last.y) > 0.04;
    if (!last || (moved && now - last.t > 80)) {
      trail.push({ x: est.x, y: est.y, yaw: est.yaw, t: now });
      if (trail.length > MAX_TRAIL) trail.splice(0, trail.length - 1600);
    }
  }

  function resetEst(seed) {
    est.x = seed && typeof seed.x === 'number' ? seed.x : 0;
    est.y = seed && typeof seed.y === 'number' ? seed.y : 0;
    est.yaw = seed && typeof seed.yaw === 'number' ? seed.yaw : 0;
    est.t = 0;
    est.mile = null;
    est.odomLive = false;
    est.lastOdomX = null;
    est.lastOdomY = null;
    est.lastOdomYaw = 0;
    est.lastOdomT = 0;
    est.lastIngestX = null;
    est.lastIngestY = null;
    est.source = '';
    est.world = false;
  }

  function onPose(a, b, c) {
    let x, y, yaw, vx = 0, vy = 0, wz = 0, imuYaw = null, mile = null;
    if (a && typeof a === 'object') {
      x = a.x;
      y = a.y;
      yaw = a.yaw;
      vx = typeof a.vx === 'number' ? a.vx : 0;
      vy = typeof a.vy === 'number' ? a.vy : 0;
      wz = typeof a.wz === 'number' ? a.wz : 0;
      imuYaw = typeof a.imuYaw === 'number' ? a.imuYaw * Math.PI / 180 : null;
      mile = typeof a.mile === 'number' ? a.mile : null;
      if (typeof a.source === 'string' && a.source && a.source !== est.source) {
        // 配准云已经锁世界系时，lio/scan 来源闪一下不能清持久图。
        if (est.source && !est.world) {
          persistMap.clear();
          frames.length = 0;
          est.lastIngestX = null;
          est.lastIngestY = null;
        }
        est.source = a.source;
      }
    } else {
      x = a;
      y = b;
      yaw = c;
    }
    if (typeof x !== 'number' || typeof y !== 'number' || typeof yaw !== 'number') {
      return;
    }

    const now = Date.now();
    const located = est.source === 'scan' || est.source === 'lio';
    const heading = located ? yaw : (imuYaw !== null ? imuYaw : yaw);

    // 第一条只锚定，不把「原点 → 当前里程计」画成一条假轨迹。
    if (est.lastOdomX === null) {
      est.x = x;
      est.y = y;
      est.yaw = heading;
      est.t = now;
      est.mile = mile;
      est.lastOdomX = x;
      est.lastOdomY = y;
      est.lastOdomYaw = yaw;
      est.lastOdomT = now;
      if (Math.hypot(x, y) > 0.05) est.odomLive = true;
      pose.x = est.x;
      pose.y = est.y;
      pose.yaw = est.yaw;
      trail.length = 0;
      trail.push({ x: est.x, y: est.y, yaw: est.yaw, t: now });
      if (opts.trail) draw();
      return;
    }

    const odomDelta = Math.hypot(x - est.lastOdomX, y - est.lastOdomY);
    if (odomDelta > 1.0) {
      trail.length = 0;
      est.x = x;
      est.y = y;
      est.yaw = heading;
      est.lastOdomX = x;
      est.lastOdomY = y;
      est.lastOdomYaw = yaw;
      est.lastOdomT = now;
      est.odomLive = true;
      pose.x = est.x;
      pose.y = est.y;
      pose.yaw = est.yaw;
      trail.push({ x: est.x, y: est.y, yaw: est.yaw, t: now });
      if (opts.trail) draw();
      return;
    }

    if (located || odomDelta > 0.015) {
      est.lastOdomX = x;
      est.lastOdomY = y;
      est.lastOdomYaw = yaw;
      est.lastOdomT = now;
      est.odomLive = true;
      est.x = x;
      est.y = y;
      est.yaw = heading;
    } else {
      // 腿式里程计冻住（RL 走路常见）。位置交给点云积分，航向跟 IMU。
      if (imuYaw !== null) est.yaw = imuYaw;
      const dt = Math.min(0.25, Math.max(0, (now - est.t) / 1000));
      const speed = Math.hypot(vx, vy);
      if (speed > 0.03) {
        applyBodyDelta(vx * dt, vy * dt, est.yaw);
      } else if (mile !== null && est.mile !== null && mile > est.mile + 2) {
        applyBodyDelta((mile - est.mile) / 100, 0, est.yaw);
      }
    }
    est.t = now;
    if (mile !== null) est.mile = mile;
    pose.x = est.x;
    pose.y = est.y;
    pose.yaw = est.yaw;
    recordTrail(now);
    if (opts.trail) draw();
  }

  function syncSubscribeButton() {
    const btn = document.getElementById('btn-cloud');
    if (!btn) return;
    btn.textContent = subscribed ? '停止点云' : '订阅点云';
    btn.classList.toggle('active', subscribed);
  }

  function paintTag(msg) {
    const tag = document.getElementById('cloud-tag');
    const note = document.getElementById('cloud-note');
    if (!tag) return;

    // active 是「网上有没有任何人在订」，不是这台平板自己。
    // 角标必须看本地 subscribed，否则 2×2 左上角会一直停在初始的「未订阅」。
    if (!subscribed) {
      if (msg && msg.error) {
        tag.textContent = '点云未启用';
        tag.className = 'tag tag-warn';
        if (note) note.textContent = msg.error;
      } else {
        tag.textContent = '未订阅';
        tag.className = 'tag';
      }
      const idle = document.getElementById('cloud-idle');
      if (idle) idle.classList.remove('hidden');
      return;
    }

    const err = msg && msg.error;
    const connected = msg ? !!msg.connected : pointCount > 0;
    if (err && !connected) {
      tag.textContent = '感知主机异常';
      tag.className = 'tag tag-warn';
      if (note) note.textContent = err;
      return;
    }
    if (!connected && pointCount === 0) {
      tag.textContent = '连接中…';
      tag.className = 'tag tag-warn';
      return;
    }

    const shown = pointCount || (msg && msg.points) || 0;
    tag.textContent = shown > 0 ? `${(shown / 1000).toFixed(1)}k 点` : '已订阅';
    tag.className = 'tag tag-ok';
    // 不写「下行 xx cm」之类诊断串，角标点数已够。
    if (note) note.textContent = '拖动旋转';
  }

  function onCloudStatus(msg) {
    lastStatus = msg;
    // 本机已订但网关说没人订：多半是重连后 client id 换了，补发一次。
    if (subscribed && msg && msg.active === false && sendFn) {
      sendFn({ t: 'cloud_sub' });
    }
    paintTag(msg);
  }

  function setSeg(rootId, attr, value) {
    const root = document.getElementById(rootId);
    if (!root) return;
    root.querySelectorAll('[' + attr + ']').forEach((b) => {
      b.classList.toggle('active', b.getAttribute(attr) === value);
    });
  }

  function syncToolbar() {
    setSeg('cloud-mode', 'data-cloud-mode', opts.persist ? 'persist' : 'live');
    const trailBtn = document.getElementById('btn-trail');
    if (trailBtn) trailBtn.classList.toggle('active', opts.trail);
    const visBtn = document.getElementById('btn-cloud-vis');
    if (visBtn) visBtn.classList.toggle('active', opts.showPoints);
    const slice = document.getElementById('cloud-slice');
    if (slice) slice.checked = opts.floorCut;
    const z = document.getElementById('cloud-slice-z');
    if (z && document.activeElement !== z) z.value = String(opts.storyH);
    const h = document.getElementById('cloud-slice-h');
    if (h && document.activeElement !== h) h.value = String(opts.displayH);
    const floorBtn = document.getElementById('btn-floor');
    if (floorBtn) {
      floorBtn.textContent = floorButtonText();
      floorBtn.classList.toggle('active', opts.floorCut && opts.floorView >= 0);
      const n = opts.floors.length;
      floorBtn.title = n
        ? `已识别 ${n} 层，点击切换；切完回到全部`
        : '先勾选楼层切割，按高度识别楼层后再切换';
    }
  }

  function applyOpts() {
    if (opts.floorCut) detectFloors();
    applyFloorView();
    syncToolbar();
    if (frames.length || persistMap.size) {
      rebuild();
      draw();
    } else {
      draw();
    }
  }

  function clearCloud(withTrail) {
    frames.length = 0;
    persistMap.clear();
    lastZMin = 0;
    zMinAt = 0;
    pointCount = 0;
    est.lastIngestX = null;
    est.lastIngestY = null;
    opts.floors = [];
    if (withTrail) trail.length = 0;
    if (gl && buffer) {
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
      gl.bufferData(gl.ARRAY_BUFFER, 0, gl.DYNAMIC_DRAW);
    }
    draw();
  }

  function applyWanted() {
    if (!sendFn) return;
    if (wanted === subscribed) return;
    subscribed = wanted;
    sendFn({ t: subscribed ? 'cloud_sub' : 'cloud_unsub' });
    syncSubscribeButton();
    paintTag(lastStatus);
    if (!subscribed) {
      // 只停下行，不清已画的点和轨迹。MESH 抖一下再回来还能接着叠，
      // 否则 App 重订会把前后两段对不齐。
      const idle = document.getElementById('cloud-idle');
      if (idle && !persistMap.size && !frames.length) {
        idle.classList.remove('hidden');
      }
    } else if (persistMap.size || frames.length) {
      const idle = document.getElementById('cloud-idle');
      if (idle) idle.classList.add('hidden');
      rebuild();
      draw();
    }
  }

  function setWanted(want) {
    wanted = !!want;
    applyWanted();
  }

  function toggle() {
    setWanted(!subscribed);
  }

  function stop() {
    setWanted(false);
  }

  // 工具条上「轨迹」「点云显控」「楼层」按钮上只有名字，看不出现在是开是关，
  // 所以按结果念，不用 voice.js 那份按字念的默认（见那边的委托监听）。
  function say(text) {
    if (window.X30Voice) window.X30Voice.say(text);
  }

  function floorSay() {
    if (!opts.floorCut) return '楼层';
    if (opts.floorView < 0) return '全部楼层';
    return '第' + (opts.floorView + 1) + '层';
  }

  function bindToolbar() {
    const bar = document.querySelector('.pane-cloud-ctl');
    if (!bar) return;
    bar.addEventListener('click', (e) => {
      const t = e.target.closest('button');
      if (!t) return;
      if (t.id === 'btn-cloud') return;
      if (t.dataset.cloudMode) {
        opts.persist = t.dataset.cloudMode === 'persist';
        if (opts.persist) {
          persistMap.clear();
          for (let i = 0; i < frames.length; i++) {
            ingestPersist(frames[i].xyz, frames[i].count, frames[i].pose,
                          !!frames[i].world);
          }
        } else {
          // 切回实时：固定 30 秒窗、10 cm 体素。
          opts.accumMs = 30000;
          opts.voxel = 0.10;
        }
        applyOpts();
        return;
      }
      if (t.id === 'btn-trail') {
        opts.trail = !opts.trail;
        applyOpts();
        say(opts.trail ? '轨迹已开' : '轨迹已关');
        return;
      }
      if (t.id === 'btn-cloud-vis') {
        // 只藏点，不退订、不清轨迹。切走点云背景才会 unsubscribe。
        opts.showPoints = !opts.showPoints;
        applyOpts();
        say(opts.showPoints ? '点云已显示' : '点云已隐藏');
        return;
      }
      if (t.id === 'btn-floor') {
        opts.floorCut = true;
        detectFloors();
        if (opts.floorView < 0) opts.floorView = 0;
        else if (opts.floorView < opts.floors.length - 1) opts.floorView += 1;
        else opts.floorView = -1;
        applyFloorView();
        applyOpts();
        say(floorSay());
        return;
      }
      if (t.id === 'btn-cloud-clear') {
        clearCloud(true);
        applyOpts();
        say('已清除点云和轨迹');
      }
    });

    const slice = document.getElementById('cloud-slice');
    if (slice) {
      slice.addEventListener('change', () => {
        opts.floorCut = slice.checked;
        if (opts.floorCut) {
          detectFloors();
          opts.floorView = -1;
        } else {
          opts.floorView = -1;
        }
        applyFloorView();
        applyOpts();
      });
    }
    const z = document.getElementById('cloud-slice-z');
    if (z) {
      z.addEventListener('change', () => {
        const v = Number(z.value);
        if (!Number.isNaN(v) && v > 0) opts.storyH = v;
        if (opts.floorCut) detectFloors();
        applyFloorView();
        applyOpts();
      });
    }
    const h = document.getElementById('cloud-slice-h');
    if (h) {
      h.addEventListener('change', () => {
        const v = Number(h.value);
        if (!Number.isNaN(v) && v > 0) {
          opts.displayH = Math.min(8, Math.max(0.2, v));
          opts.sliceHalf = opts.displayH;
        }
        applyFloorView();
        applyOpts();
      });
    }
  }

  function initCloud(send) {
    sendFn = send;
    lastStatus = null;
    trail.length = 0;
    resetEst();
    clearCloud();
    if (!gl && !initGl()) {
      applyWanted();
      return;
    }
    bindToolbar();
    syncSubscribeButton();
    paintTag(null);
    syncToolbar();
    // applyLayout 可能先于 init 把 wanted 置上，这里补发订阅。
    applyWanted();
  }

  function resubscribe() {
    if (!subscribed || !sendFn) return;
    sendFn({ t: 'cloud_sub' });
  }

  /** 网关回 no_cloud：退回未订阅，角标写清原因（设置里开「启用点云回传」）。 */
  function onDenied(reason) {
    wanted = false;
    subscribed = false;
    lastStatus = { active: false, connected: false, error: reason || '网关未启用点云' };
    clearCloud();
    syncSubscribeButton();
    paintTag(lastStatus);
    const idle = document.getElementById('cloud-idle');
    const small = idle ? idle.querySelector('small') : null;
    if (small) {
      small.textContent = reason ||
        '网关未启用点云。设置 → 启用点云回传，并确认感知主机 ROS 可达';
    }
    if (idle) idle.classList.remove('hidden');
  }

  function nudgeZoom(steps) {
    if (!steps) return;
    cam.dist *= Math.pow(0.89, steps);
    cam.dist = Math.max(2, Math.min(60, cam.dist));
    if (gl) draw();
  }

  // 左小摇杆平移显示中心。ax 左为正、ay 上为正，按镜头水平方向在地面上挪。
  function nudgeLook(ax, ay) {
    if (!ax && !ay) return;
    const cy = Math.cos(cam.yaw), sy = Math.sin(cam.yaw);
    const rx = sy, ry = -cy;
    const fx = -cy, fy = -sy;
    const speed = Math.max(4, cam.dist) * 0.045;
    cam.lookX += (-ax) * rx * speed + ay * fx * speed;
    cam.lookY += (-ax) * ry * speed + ay * fy * speed;
    cam.lookX = Math.max(-150, Math.min(150, cam.lookX));
    cam.lookY = Math.max(-150, Math.min(150, cam.lookY));
    if (gl) draw();
  }

  window.X30Cloud = {
    initCloud, onCloudFrame, onCloudStatus, onPose, stop, resize, resubscribe,
    onDenied, setWanted, nudgeZoom, nudgeLook, setSoldiers,
  };
})();
