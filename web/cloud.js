// 点云渲染。WebGL 直接画，不引 three.js。
//
// 网关下发的是机体系当前帧（见 docs/app-protocol.md），不是累积地图。
// 累积、楼层切割、显示体素、轨迹都在本机做：用运动主机里程计把历史帧
// 变到当前机体系，不回头向感知主机多要带宽。
//
// 坐标仍按量化帧还原：世界（机体）坐标 = origin + uint16 × scale。

(function () {
  'use strict';

  const MAGIC = 0x43303358; // "X30C" 小端读成 u32
  const HEADER_SIZE = 40;
  const FLOORS = [0, 3, 6, 9];
  const MAX_LIVE_FRAMES = 80;
  const MAX_PERSIST = 80000;
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
  const persistMap = new Map();
  const est = { x: 0, y: 0, yaw: 0, t: 0, mile: null, odomLive: false };

  const opts = {
    persist: false,
    accumMs: 0,
    voxel: 0.10,
    trail: true,
    slice: false,
    sliceZ: 0,
    sliceHalf: 0.6,
    floorIdx: 0,
  };

  // 相机：绕原点的轨道视角。仰角限制在两极之间，避免翻转。
  const cam = { yaw: -2.4, pitch: 0.5, dist: 12 };
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
      gl_PointSize = clamp(u_size / gl_Position.w, 1.0, 4.0);
    }
  `;

  // 按高度着色。用高度而不是强度，是因为遥控员真正要判断的是
  // "前面那团东西是地面起伏还是一堵墙"，高度直接回答这个问题。
  const FRAG = `
    precision mediump float;
    varying float v_h;
    uniform float u_slice;
    uniform float u_z0;
    uniform float u_zHalf;
    void main() {
      if (u_slice > 0.5 && abs(v_h - u_z0) > u_zHalf) discard;
      float t = clamp((v_h + 0.6) / 2.6, 0.0, 1.0);
      vec3 low  = vec3(0.15, 0.45, 0.75);
      vec3 mid  = vec3(0.30, 0.85, 0.60);
      vec3 high = vec3(0.98, 0.80, 0.25);
      vec3 c = t < 0.5 ? mix(low, mid, t * 2.0) : mix(mid, high, (t - 0.5) * 2.0);
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
    gl.clearColor(0.05, 0.07, 0.10, 1.0);
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
      cam.dist * cp * cy,
      cam.dist * cp * sy,
      cam.dist * sp + 1.0,
    ];
    const target = [0, 0, 0];
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

  function ingestPersist(xyz, n, from) {
    const inv = 1 / Math.max(opts.voxel, 0.05);
    for (let i = 0; i < n && persistMap.size < MAX_PERSIST; i++) {
      const w = bodyToOdom(xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2], from);
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
        const samePose = Math.abs(f.pose.x - now.x) < 1e-4 &&
                         Math.abs(f.pose.y - now.y) < 1e-4 &&
                         Math.abs(f.pose.yaw - now.yaw) < 1e-4;
        for (let k = 0; k < f.count; k++) {
          const x = f.xyz[k * 3], y = f.xyz[k * 3 + 1], z = f.xyz[k * 3 + 2];
          if (samePose) {
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

  function trailBodyVerts() {
    const out = [];
    for (let i = 0; i < trail.length; i++) {
      const b = odomToBody(trail[i].x, trail[i].y, 0.12, pose);
      out.push(b[0], b[1], b[2]);
      // 段太长时中间补点，WebGL 线宽在很多 GPU 上只能是 1px。
      if (i + 1 < trail.length) {
        const n = odomToBody(trail[i + 1].x, trail[i + 1].y, 0.12, pose);
        const dx = n[0] - b[0], dy = n[1] - b[1];
        const dist = Math.hypot(dx, dy);
        const steps = Math.min(8, Math.floor(dist / 0.08));
        for (let s = 1; s < steps; s++) {
          const t = s / steps;
          out.push(b[0] + dx * t, b[1] + dy * t, 0.12);
        }
      }
    }
    return new Float32Array(out);
  }

  function headingVerts() {
    return new Float32Array([
      0, 0, 0.12, 0.55, 0, 0.12,
      0.55, 0, 0.12, 0.38, 0.10, 0.12,
      0.55, 0, 0.12, 0.38, -0.10, 0.12,
    ]);
  }

  function draw() {
    if (!gl || !program) return;
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    const mvp = mvpMatrix(canvas.width / canvas.height);

    if (pointCount > 0) {
      gl.useProgram(program);
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
      const loc = gl.getAttribLocation(program, 'a_p');
      gl.enableVertexAttribArray(loc);
      gl.vertexAttribPointer(loc, 3, gl.FLOAT, false, 0, 0);
      gl.uniform1f(gl.getUniformLocation(program, 'u_size'), canvas.height / 180);
      gl.uniformMatrix4fv(gl.getUniformLocation(program, 'u_mvp'), false, mvp);
      gl.uniform1f(gl.getUniformLocation(program, 'u_slice'), opts.slice ? 1 : 0);
      gl.uniform1f(gl.getUniformLocation(program, 'u_z0'), opts.sliceZ);
      gl.uniform1f(gl.getUniformLocation(program, 'u_zHalf'), opts.sliceHalf);
      gl.drawArrays(gl.POINTS, 0, pointCount);
    }

    if (!opts.trail || !subscribed) return;

    gl.disable(gl.DEPTH_TEST);
    gl.useProgram(lineProgram);
    const loc = gl.getAttribLocation(lineProgram, 'a_p');
    gl.enableVertexAttribArray(loc);
    gl.uniformMatrix4fv(gl.getUniformLocation(lineProgram, 'u_mvp'), false, mvp);
    gl.uniform3f(gl.getUniformLocation(lineProgram, 'u_color'), 1.0, 0.16, 0.16);

    const dots = trailBodyVerts();
    const nDots = dots.length / 3;
    if (nDots > 0) {
      gl.bindBuffer(gl.ARRAY_BUFFER, lineBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, dots, gl.DYNAMIC_DRAW);
      gl.vertexAttribPointer(loc, 3, gl.FLOAT, false, 0, 0);
      gl.uniform1f(gl.getUniformLocation(lineProgram, 'u_size'), 6.0);
      if (nDots >= 2) gl.drawArrays(gl.LINE_STRIP, 0, nDots);
      gl.drawArrays(gl.POINTS, 0, nDots);
    }

    const head = headingVerts();
    gl.bindBuffer(gl.ARRAY_BUFFER, lineBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, head, gl.DYNAMIC_DRAW);
    gl.vertexAttribPointer(loc, 3, gl.FLOAT, false, 0, 0);
    gl.uniform1f(gl.getUniformLocation(lineProgram, 'u_size'), 8.0);
    gl.drawArrays(gl.LINES, 0, 6);
    gl.enable(gl.DEPTH_TEST);
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
    const stamped = {
      t: Date.now(),
      pose: { x: pose.x, y: pose.y, yaw: pose.yaw },
      xyz: xyz,
      count: count,
    };
    frames.push(stamped);
    if (frames.length > MAX_LIVE_FRAMES) frames.splice(0, frames.length - MAX_LIVE_FRAMES);
    if (opts.persist) ingestPersist(xyz, count, stamped.pose);

    // 帧到了就是订上了。只信本地按钮的话，重连或 2×2 切布局时角标会停在「未订阅」。
    if (!subscribed) {
      subscribed = true;
      syncSubscribeButton();
    }
    const idle = document.getElementById('cloud-idle');
    if (idle) idle.classList.add('hidden');
    rebuild();
    draw();
    paintTag(lastStatus);
  }

  function resetEst(seed) {
    est.x = seed && typeof seed.x === 'number' ? seed.x : 0;
    est.y = seed && typeof seed.y === 'number' ? seed.y : 0;
    est.yaw = seed && typeof seed.yaw === 'number' ? seed.yaw : 0;
    est.t = 0;
    est.mile = null;
    est.odomLive = false;
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
    } else {
      x = a;
      y = b;
      yaw = c;
    }
    if (typeof x !== 'number' || typeof y !== 'number' || typeof yaw !== 'number') {
      return;
    }
    if (!subscribed) return;

    const now = Date.now();
    if (!est.t) {
      resetEst({ x: x, y: y, yaw: yaw });
      est.t = now;
      est.mile = mile;
    }

    const dt = Math.min(0.25, Math.max(0, (now - est.t) / 1000));
    est.t = now;

    const dOdom = Math.hypot(x - est.x, y - est.y);
    if (Math.hypot(x, y) > 0.05 && dOdom > 0.015) est.odomLive = true;

    if (est.odomLive) {
      est.x = x;
      est.y = y;
      est.yaw = yaw;
    } else {
      const heading = imuYaw !== null ? imuYaw : yaw;
      const c = Math.cos(heading), s = Math.sin(heading);
      const speed = Math.hypot(vx, vy);
      if (speed > 0.03) {
        est.x += (c * vx - s * vy) * dt;
        est.y += (s * vx + c * vy) * dt;
      } else if (mile !== null && est.mile !== null && mile > est.mile + 2) {
        // 速度遥测也是 0 时，用本次里程沿机头往前推。
        const ds = (mile - est.mile) / 100;
        est.x += c * ds;
        est.y += s * ds;
      }
      est.yaw = heading + wz * dt;
    }
    if (mile !== null) est.mile = mile;

    pose.x = est.x;
    pose.y = est.y;
    pose.yaw = est.yaw;

    const last = trail[trail.length - 1];
    const moved = !last ||
      (est.x - last.x) * (est.x - last.x) + (est.y - last.y) * (est.y - last.y) > 0.0004 ||
      Math.abs(est.yaw - last.yaw) > 0.03;
    const speeding = Math.hypot(vx, vy) > 0.04;
    if (!last || ((moved || speeding) && now - last.t > 80)) {
      trail.push({ x: est.x, y: est.y, yaw: est.yaw, t: now });
      if (trail.length > MAX_TRAIL) trail.splice(0, trail.length - 1600);
    }
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
      tag.textContent = '未订阅';
      tag.className = 'tag';
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
    if (note && msg) {
      const dropped = msg.dropped > 0 ? `，丢帧 ${msg.dropped}` : '';
      const extra = opts.persist ? '，持久' :
                    opts.accumMs > 0 ? `，累积 ${opts.accumMs / 1000}s` : '';
      note.textContent = `下行 ${(msg.voxel * 100).toFixed(0)} cm${extra}${dropped}`;
    }
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
    setSeg('cloud-accum', 'data-cloud-accum', String(opts.accumMs / 1000));
    setSeg('cloud-voxel', 'data-cloud-voxel', opts.voxel.toFixed(2));
    const accum = document.getElementById('cloud-accum');
    if (accum) accum.classList.toggle('is-dim', opts.persist);
    const trailBtn = document.getElementById('btn-trail');
    if (trailBtn) trailBtn.classList.toggle('active', opts.trail);
    const slice = document.getElementById('cloud-slice');
    if (slice) slice.checked = opts.slice;
    const z = document.getElementById('cloud-slice-z');
    if (z && document.activeElement !== z) z.value = String(opts.sliceZ);
    const h = document.getElementById('cloud-slice-h');
    if (h && document.activeElement !== h) h.value = String(opts.sliceHalf);
    const clear = document.getElementById('btn-cloud-clear');
    if (clear) clear.classList.toggle('hidden', !opts.persist);
  }

  function applyOpts() {
    syncToolbar();
    if (frames.length || persistMap.size) {
      rebuild();
      draw();
    } else {
      draw();
    }
  }

  function clearCloud() {
    frames.length = 0;
    persistMap.clear();
    pointCount = 0;
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
      trail.length = 0;
      resetEst();
      clearCloud();
      const idle = document.getElementById('cloud-idle');
      if (idle) idle.classList.remove('hidden');
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
            ingestPersist(frames[i].xyz, frames[i].count, frames[i].pose);
          }
        }
        applyOpts();
        return;
      }
      if (t.dataset.cloudAccum !== undefined) {
        opts.accumMs = Number(t.dataset.cloudAccum) * 1000;
        applyOpts();
        return;
      }
      if (t.dataset.cloudVoxel) {
        const next = Number(t.dataset.cloudVoxel);
        if (next !== opts.voxel) {
          opts.voxel = next;
          if (opts.persist) {
            const old = [];
            persistMap.forEach((w) => old.push(w));
            persistMap.clear();
            const inv = 1 / Math.max(opts.voxel, 0.05);
            for (let i = 0; i < old.length && persistMap.size < MAX_PERSIST; i++) {
              const w = old[i];
              persistMap.set(voxelKey(w[0], w[1], w[2], inv), w);
            }
          }
        }
        applyOpts();
        return;
      }
      if (t.id === 'btn-trail') {
        opts.trail = !opts.trail;
        applyOpts();
        return;
      }
      if (t.id === 'btn-floor') {
        opts.floorIdx = (opts.floorIdx + 1) % FLOORS.length;
        opts.sliceZ = FLOORS[opts.floorIdx];
        opts.slice = true;
        applyOpts();
        return;
      }
      if (t.id === 'btn-cloud-clear') {
        persistMap.clear();
        frames.length = 0;
        applyOpts();
      }
    });

    const slice = document.getElementById('cloud-slice');
    if (slice) {
      slice.addEventListener('change', () => {
        opts.slice = slice.checked;
        applyOpts();
      });
    }
    const z = document.getElementById('cloud-slice-z');
    if (z) {
      z.addEventListener('change', () => {
        const v = Number(z.value);
        if (!Number.isNaN(v)) opts.sliceZ = v;
        applyOpts();
      });
    }
    const h = document.getElementById('cloud-slice-h');
    if (h) {
      h.addEventListener('change', () => {
        const v = Number(h.value);
        if (!Number.isNaN(v) && v > 0) opts.sliceHalf = v;
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

  function nudgeZoom(steps) {
    if (!steps) return;
    cam.dist *= Math.pow(0.89, steps);
    cam.dist = Math.max(2, Math.min(60, cam.dist));
    if (gl) draw();
  }

  window.X30Cloud = {
    initCloud, onCloudFrame, onCloudStatus, onPose, stop, resize, resubscribe,
    setWanted, nudgeZoom,
  };
})();
