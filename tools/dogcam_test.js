// 2.4G 机身相机画面的单元测试：node tools/dogcam_test.js
//
// 这一层值得单独测，因为它错起来只有一种表现 —— 黑屏，而黑屏的可能原因太多
// （网关、相机、网卡、链路），现场很难往「网页少发了一句 videoStart」上想。
// 这里用假的 DOM 和假的原生桥把状态机跑一遍，不需要平板和狗。

'use strict';

var fs = require('fs');
var path = require('path');
var vm = require('vm');

var pass = 0, fail = 0;

function check(name, ok, detail) {
  if (ok) { pass++; console.log('  [OK]   ' + name); }
  else { fail++; console.log('  [FAIL] ' + name + (detail ? '  ' + detail : '')); }
}

// 只造这个模块真正摸到的那几个节点，多了反而看不清它依赖什么。
function harness() {
  var h = {
    calls: [],
    cls: {},
    on24: false,
    hidden: false,
    nodes: {},
  };

  function node(text) {
    var small = { textContent: text };
    return {
      small: small,
      hiddenNow: false,
      classList: {
        contains: function () { return false; },
        add: function () {}, remove: function () {},
        toggle: function (c, on) { this.owner.hiddenNow = !!on; },
      },
      querySelector: function () { return small; },
      getBoundingClientRect: function () {
        return { left: 0, top: 64, width: 1280, height: 600 };
      },
    };
  }

  h.nodes['pane-video'] = node('');
  h.nodes['media-idle'] = node('等待拉流…');
  h.nodes['media-idle-ptz-vis'] = node('未接通时会停在这里');
  h.nodes['media-idle-ptz-ir'] = node('未接通时会停在这里');
  Object.keys(h.nodes).forEach(function (k) {
    h.nodes[k].classList.owner = h.nodes[k];
  });
  // 平板壳里机身相机就是当前主画面。
  h.nodes['pane-video'].classList.contains = function (c) { return c === 'is-main'; };

  var store = {};
  var sandbox = {
    window: {
      devicePixelRatio: 2,
      localStorage: {
        getItem: function (k) { return store[k] || null; },
        setItem: function (k, v) { store[k] = v; },
        removeItem: function (k) { delete store[k]; },
      },
      addEventListener: function () {},
      setInterval: function () { return 0; },
      X30Native: {
        videoStart: function (u) { h.calls.push('start ' + u); },
        videoStop: function () { h.calls.push('stop'); },
        videoRect: function (x, y, w, hh) {
          h.calls.push('rect ' + x + ',' + y + ',' + w + ',' + hh);
        },
      },
    },
    document: {
      addEventListener: function () {},
      documentElement: {
        classList: {
          add: function (c) { h.cls[c] = true; },
          remove: function (c) { delete h.cls[c]; },
          contains: function (c) {
            return c === 'radio-24' ? h.on24 : !!h.cls[c];
          },
        },
      },
      getElementById: function (id) { return h.nodes[id] || null; },
    },
  };
  Object.defineProperty(sandbox.document, 'hidden', {
    get: function () { return h.hidden; },
  });

  var src = fs.readFileSync(
    path.join(__dirname, '..', 'web', 'dogcam24.js'), 'utf8');
  vm.runInNewContext(src, sandbox, { filename: 'dogcam24.js' });
  h.mod = sandbox.window.X30DogCam;
  h.idle = function () { return h.nodes['media-idle'].small.textContent; };
  h.ptz = function () { return h.nodes['media-idle-ptz-vis'].small.textContent; };
  return h;
}

// --- MESH 下必须完全不插手 ---------------------------------------------------
console.log('\n== MESH 下不碰这一路 ==');

var h = harness();
h.mod.init();
check('不去开原生播放器', h.calls.length === 0, JSON.stringify(h.calls));
check('不改布控球那两格的说明', h.ptz() === '未接通时会停在这里', h.ptz());
check('不把网页背景透掉', !h.cls['native-video-on']);

// --- 切到 2.4G ---------------------------------------------------------------
console.log('\n== 切到 2.4G ==');

h.on24 = true;
h.mod.onRadioPath();
// 先报矩形再开流：反过来的话原生会先按上一次（或整屏）的位置画一下，画面会跳。
check('先报矩形，再开流',
      h.calls[0] === 'rect 0,128,2560,1200' &&
      h.calls[1] === 'start rtsp://192.168.1.105:8554/test',
      JSON.stringify(h.calls));
// 网页是 CSS 像素、原生是设备像素，中间差一个 devicePixelRatio（这里是 2）。
check('矩形按 devicePixelRatio 换算成设备像素',
      h.calls[0] === 'rect 0,128,2560,1200', h.calls[0]);
check('把网页背景透出去，否则看不到底下的画面', !!h.cls['native-video-on']);
check('占位图说明正在拉哪个地址',
      h.idle().indexOf('rtsp://192.168.1.105:8554/test') !== -1, h.idle());
check('布控球如实标成 2.4G 下不可用',
      h.ptz().indexOf('布控球在网关那侧') !== -1, h.ptz());

// 切布局、切档、定时器都会走到 sync。每次都重开流的话画面会不停闪断。
h.calls.length = 0;
h.mod.onStageLayout();
h.mod.onRadioPath();
h.mod.onStageLayout();
check('反复同步不会把正在放的流打断', h.calls.length === 0, JSON.stringify(h.calls));

// --- 出错和出画面 -----------------------------------------------------------
console.log('\n== 状态回报 ==');

h.mod.onState({ playing: false, err: 'Unable to connect to /192.168.1.105:8554' });
check('拉不到时占位图写出原生的报错',
      h.idle().indexOf('Unable to connect') !== -1, h.idle());
check('拉不到时占位图还留着', !h.nodes['media-idle'].hiddenNow);

h.mod.onState({ playing: true, err: '' });
check('出画面后收起占位图', h.nodes['media-idle'].hiddenNow);

// --- 切后台 -----------------------------------------------------------------
console.log('\n== 切后台 ==');

h.calls.length = 0;
h.hidden = true;
h.mod.stop();
check('停流并把背景恢复成不透明',
      h.calls.length === 1 && h.calls[0] === 'stop' && !h.cls['native-video-on'],
      JSON.stringify(h.calls));
h.hidden = false;
h.mod.onRadioPath();
check('回前台重新开流',
      h.calls.indexOf('start rtsp://192.168.1.105:8554/test') !== -1,
      JSON.stringify(h.calls));

// --- 切回 MESH --------------------------------------------------------------
console.log('\n== 切回 MESH ==');

h.calls.length = 0;
h.on24 = false;
h.mod.onRadioPath();
check('停掉原生这一路', h.calls.length === 1 && h.calls[0] === 'stop',
      JSON.stringify(h.calls));
check('背景恢复不透明，否则 MESH 的 WebRTC 画面会被黑底盖住',
      !h.cls['native-video-on']);
// 占位图交回 media.js 管，留着 2.4G 的话会让人以为还在走那条路。
check('占位图说明还原', h.idle() === '等待拉流…', h.idle());
check('布控球说明还原', h.ptz() === '未接通时会停在这里', h.ptz());

// --- 地址可改 ---------------------------------------------------------------
console.log('\n== 相机地址 ==');

check('默认是感知主机那一路', h.mod.url() === 'rtsp://192.168.1.105:8554/test');
h.mod.setUrl('rtsp://192.168.1.200:8554/live');
check('改过之后记得住', h.mod.url() === 'rtsp://192.168.1.200:8554/live', h.mod.url());
h.mod.setUrl('');
check('清空回到默认', h.mod.url() === 'rtsp://192.168.1.105:8554/test', h.mod.url());

h.on24 = true;
h.mod.onRadioPath();
h.calls.length = 0;
h.mod.setUrl('rtsp://192.168.1.200:8554/live');
check('放着的时候改地址会按新地址重开',
      h.calls.indexOf('stop') !== -1 &&
      h.calls.indexOf('start rtsp://192.168.1.200:8554/live') !== -1,
      JSON.stringify(h.calls));

console.log('\n通过 ' + pass + '，失败 ' + fail);
process.exit(fail === 0 ? 0 : 1);
