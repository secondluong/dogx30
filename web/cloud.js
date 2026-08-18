// 点云渲染。WebGL 直接画，不引 three.js。
//
// 不引库有两个实在的理由：一是遥控端要能离线装，多一个几百 KB 的依赖就多一份
// 打包和版本维护；二是我们只需要「画一堆点」这一个功能，three.js 的场景图、
// 材质、光照全用不上。整个渲染就是一个 attribute 加两个 uniform。
//
// 关键设计：GPU 里直接吃 int16。网关下发的就是量化后的 int16，如果在 JS 里
// 先转成 float32 再上传，等于白白多一次遍历和一倍显存 —— 用
// vertexAttribPointer 的归一化整型格式，让顶点着色器自己还原坐标。

(function () {
  'use strict';

  const MAGIC = 0x43303358; // "X30C" 小端读成 u32
  const HEADER_SIZE = 40;

  let gl = null;
  let program = null;
  let buffer = null;
  let canvas = null;
  let pointCount = 0;
  let origin = [0, 0, 0];
  let scale = 1;
  let subscribed = false;
  let sendFn = null;

  // 相机：绕原点的轨道视角。仰角限制在两极之间，避免翻转。
  const cam = { yaw: -2.4, pitch: 0.5, dist: 12 };
  let dragging = false;
  let lastX = 0;
  let lastY = 0;

  const VERT = `
    attribute vec3 a_q;
    uniform vec3 u_origin;
    uniform float u_scale;
    uniform mat4 u_mvp;
    uniform float u_size;
    varying float v_h;
    void main() {
      // a_q 是归一化过的 uint16（0..1），还原成量化前的坐标
      vec3 p = u_origin + a_q * 65535.0 * u_scale;
      v_h = p.z;
      gl_Position = u_mvp * vec4(p, 1.0);
      // 近处的点画大一些，远处收小，避免远景糊成一团白
      gl_PointSize = clamp(u_size / gl_Position.w, 1.0, 4.0);
    }
  `;

  // 按高度着色。用高度而不是强度，是因为遥控员真正要判断的是
  // "前面那团东西是地面起伏还是一堵墙"，高度直接回答这个问题。
  const FRAG = `
    precision mediump float;
    varying float v_h;
    void main() {
      float t = clamp((v_h + 0.6) / 2.6, 0.0, 1.0);
      vec3 low  = vec3(0.15, 0.45, 0.75);
      vec3 mid  = vec3(0.30, 0.85, 0.60);
      vec3 high = vec3(0.98, 0.80, 0.25);
      vec3 c = t < 0.5 ? mix(low, mid, t * 2.0) : mix(mid, high, (t - 0.5) * 2.0);
      gl_FragColor = vec4(c, 1.0);
    }
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

  function initGl() {
    canvas = document.getElementById('cloud-canvas');
    if (!canvas) return false;
    gl = canvas.getContext('webgl', { antialias: false, alpha: false });
    if (!gl) {
      const note = document.getElementById('cloud-note');
      if (note) note.textContent = '此设备不支持 WebGL，点云无法显示';
      return false;
    }

    const vs = compile(gl.VERTEX_SHADER, VERT);
    const fs = compile(gl.FRAGMENT_SHADER, FRAG);
    if (!vs || !fs) return false;

    program = gl.createProgram();
    gl.attachShader(program, vs);
    gl.attachShader(program, fs);
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      console.error('着色器链接失败:', gl.getProgramInfoLog(program));
      return false;
    }

    buffer = gl.createBuffer();
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

  function draw() {
    if (!gl || !program) return;
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    if (pointCount === 0) return;

    gl.useProgram(program);
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);

    const loc = gl.getAttribLocation(program, 'a_q');
    gl.enableVertexAttribArray(loc);
    // normalized=true 让 GPU 把 uint16 映射到 0..1，省掉 CPU 侧的转换
    gl.vertexAttribPointer(loc, 3, gl.UNSIGNED_SHORT, true, 6, 0);

    gl.uniform3fv(gl.getUniformLocation(program, 'u_origin'), origin);
    gl.uniform1f(gl.getUniformLocation(program, 'u_scale'), scale);
    gl.uniform1f(gl.getUniformLocation(program, 'u_size'),
                 canvas.height / 180);
    gl.uniformMatrix4fv(gl.getUniformLocation(program, 'u_mvp'), false,
                        mvpMatrix(canvas.width / canvas.height));

    gl.drawArrays(gl.POINTS, 0, pointCount);
  }

  function onCloudFrame(arrayBuffer) {
    const view = new DataView(arrayBuffer);
    if (arrayBuffer.byteLength < HEADER_SIZE) return;
    if (view.getUint32(0, true) !== MAGIC) return;
    if (view.getUint8(4) !== 1) return;

    origin = [view.getFloat32(20, true), view.getFloat32(24, true),
              view.getFloat32(28, true)];
    scale = view.getFloat32(32, true);
    const count = view.getUint32(36, true);
    if (HEADER_SIZE + count * 6 > arrayBuffer.byteLength) return;

    if (!gl) return;
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER,
                  new Uint8Array(arrayBuffer, HEADER_SIZE, count * 6),
                  gl.DYNAMIC_DRAW);
    pointCount = count;

    const idle = document.getElementById('cloud-idle');
    if (idle) idle.classList.add('hidden');
    draw();
  }

  function onCloudStatus(msg) {
    const tag = document.getElementById('cloud-tag');
    const note = document.getElementById('cloud-note');
    if (!tag) return;

    if (!msg.active) {
      tag.textContent = '未订阅';
      tag.className = 'tag';
    } else if (msg.error) {
      tag.textContent = '感知主机异常';
      tag.className = 'tag tag-warn';
      if (note) note.textContent = msg.error;
    } else if (!msg.connected) {
      tag.textContent = '连接中…';
      tag.className = 'tag tag-warn';
    } else {
      tag.textContent = `${(msg.points / 1000).toFixed(1)}k 点`;
      tag.className = 'tag tag-ok';
      if (note) {
        const dropped = msg.dropped > 0 ? `，丢帧 ${msg.dropped}` : '';
        note.textContent =
          `体素 ${(msg.voxel * 100).toFixed(0)} cm${dropped}`;
      }
    }
  }

  function toggle() {
    if (!sendFn) return;
    subscribed = !subscribed;
    sendFn({ t: subscribed ? 'cloud_sub' : 'cloud_unsub' });
    const btn = document.getElementById('btn-cloud');
    if (btn) {
      btn.textContent = subscribed ? '停止点云' : '订阅点云';
      btn.classList.toggle('active', subscribed);
    }
    if (!subscribed) {
      pointCount = 0;
      const idle = document.getElementById('cloud-idle');
      if (idle) idle.classList.remove('hidden');
      draw();
    }
  }

  function stop() {
    if (subscribed) toggle();
  }

  function initCloud(send) {
    sendFn = send;
    if (!gl && !initGl()) return;
    const btn = document.getElementById('btn-cloud');
    if (btn) btn.addEventListener('click', toggle);
    // 重连后网关那边不记得我们订阅过，状态要跟着回到未订阅。
    subscribed = false;
    pointCount = 0;
    if (btn) {
      btn.textContent = '订阅点云';
      btn.classList.remove('active');
    }
  }

  window.X30Cloud = { initCloud, onCloudFrame, onCloudStatus, stop };
})();
