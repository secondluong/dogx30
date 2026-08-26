// 按键语音播报测试：node tools/voice_test.js
//
// 这个模块最容易出的两类错，都不是「念得不好听」，而是**念错**：
//
// 一、按钮被拦下了还照着按钮上的字念。人听到「起立」就以为发出去了，实际上没有
//     控制权、或者链路不通，狗一动不动 —— 语音比屏幕更容易被当真，念错比不念糟。
//     所以设计上是同一拍里后念的覆盖先念的，这份覆盖必须有测试盯着。
// 二、在 WebView 里以为自己念了。安卓 WebView 没有 speechSynthesis，但那个对象
//     有时**是存在的**，speak() 静默不出声。所以有原生桥时必须走原生，只在原生
//     真的不接（旧安装包）时才退回浏览器合成。
//
// 这两件事在电脑浏览器上都表现正常，只有装到平板上、或者现场按下去才会暴露。

'use strict';

var path = require('path');

var pass = 0, fail = 0;

function check(name, ok, detail) {
  if (ok) { pass++; console.log('  [OK]   ' + name); }
  else { fail++; console.log('  [FAIL] ' + name + (detail ? '  ' + detail : '')); }
}

// --- 假 DOM ----------------------------------------------------------------

var timers = [];
var docHandlers = {};
var elements = {};
var storage = {};
var nativeSaid = [];
var webSaid = [];
var nativeThrows = false;
var nativeStatus = 'ok';
var talkOn = false;

function fakeEl(attrs, text) {
  return {
    attrs: attrs || {},
    textContent: text || '',
    checked: false,
    disabled: false,
    _handlers: {},
    getAttribute: function (k) {
      return Object.prototype.hasOwnProperty.call(this.attrs, k) ? this.attrs[k] : null;
    },
    addEventListener: function (t, fn) { this._handlers[t] = fn; },
    change: function (v) {
      this.checked = v;
      if (this._handlers.change) this._handlers.change();
    },
    // 委托监听是 e.target.closest('button, [data-say]')，能匹配的元素返回自己。
    closest: function () { return this.matches === false ? null : this; },
  };
}

function setup(opts) {
  var o = opts || {};
  timers = [];
  docHandlers = {};
  elements = {};
  nativeSaid = [];
  webSaid = [];
  nativeThrows = !!o.nativeThrows;
  nativeStatus = o.nativeStatus || 'ok';
  talkOn = false;

  elements['set-voice'] = fakeEl();
  elements['set-voice-hint'] = fakeEl();

  global.document = {
    getElementById: function (id) { return elements[id] || null; },
    addEventListener: function (t, fn) { docHandlers[t] = fn; },
  };

  global.window = {
    localStorage: {
      getItem: function (k) {
        return Object.prototype.hasOwnProperty.call(storage, k) ? storage[k] : null;
      },
      setItem: function (k, v) { storage[k] = v; },
      removeItem: function (k) { delete storage[k]; },
    },
    setTimeout: function (fn) { timers.push(fn); return timers.length; },
    clearTimeout: function (id) { timers[id - 1] = null; },
    speechSynthesis: o.noWebSynth ? null : {
      cancel: function () {},
      speak: function (u) { webSaid.push(u.text); },
    },
    SpeechSynthesisUtterance: o.noWebSynth ? null : function (text) {
      this.text = text;
    },
    X30Media: { talking: function () { return talkOn; } },
  };
  if (!o.noNative) {
    global.window.X30Native = {
      speak: function (text) {
        if (nativeThrows) throw new Error('旧安装包没有这个桥方法');
        nativeSaid.push(text);
        return true;
      },
      ttsStop: function () {},
      ttsStatus: function () { return nativeStatus; },
    };
  }
  global.self = global.window;

  delete require.cache[require.resolve(path.join(__dirname, '..', 'web', 'voice.js'))];
  require(path.join(__dirname, '..', 'web', 'voice.js'));
  global.window.X30Voice.initVoice();
  return global.window.X30Voice;
}

// 一拍：把这一轮排队的合成任务放出去。真实浏览器里就是当前这个宏任务跑完。
function tick() {
  var q = timers;
  timers = [];
  for (var i = 0; i < q.length; i++) {
    if (q[i]) q[i]();
  }
}

function clickOn(el) {
  docHandlers.click({ target: el });
}

// ---------------------------------------------------------------------------
console.log('\n== 念按钮上的字 ==');
// ---------------------------------------------------------------------------

var V = setup();
clickOn(fakeEl({}, '起立'));
tick();
check('默认念按钮上的字', nativeSaid.join('|') === '起立', nativeSaid.join('|'));

V = setup();
clickOn(fakeEl({ 'data-say': '设置' }, 'X30 遥控台'));
tick();
check('data-say 覆盖按钮上的字', nativeSaid.join('|') === '设置', nativeSaid.join('|'));

V = setup();
clickOn(fakeEl({ 'data-say': '' }, 'MESH'));
tick();
check('data-say 为空的按钮不念（字是当前状态，念了正好相反）',
      nativeSaid.length === 0, nativeSaid.join('|'));

V = setup();
clickOn(fakeEl({ 'aria-label': '步态' }, '常规 ›'));
tick();
check('按钮里带箭头时念 aria-label',
      nativeSaid.join('|') === '步态', nativeSaid.join('|'));

V = setup();
var plain = fakeEl({}, '已连接');
plain.matches = false;
clickOn(plain);
tick();
check('点到不是按钮的东西不念', nativeSaid.length === 0, nativeSaid.join('|'));

V = setup();
var off = fakeEl({}, '常规');
off.disabled = true;
clickOn(off);
tick();
check('禁用的按钮不念', nativeSaid.length === 0, nativeSaid.join('|'));

// ---------------------------------------------------------------------------
console.log('\n== 被拦下时改口，且只出一句 ==');
// ---------------------------------------------------------------------------
// 委托监听在捕获阶段先念按钮上的字，处理函数随后发现发不出去，再念一次真实结果。
// 两句都念出来的话，人听到的是「起立…没有控制权」，第一句已经让他松了手。

V = setup();
clickOn(fakeEl({}, '起立'));      // 捕获阶段
V.say('没有控制权');               // 处理函数改口
tick();
check('同一拍里后念的盖掉先念的',
      nativeSaid.join('|') === '没有控制权', nativeSaid.join('|'));

V = setup();
clickOn(fakeEl({}, '起立'));
tick();
V.say('没有控制权');
tick();
check('隔了一拍就是两句，不会被误吞',
      nativeSaid.join('|') === '起立|没有控制权', nativeSaid.join('|'));

// ---------------------------------------------------------------------------
console.log('\n== 挑哪个引擎念 ==');
// ---------------------------------------------------------------------------

V = setup();
V.say('起立');
tick();
check('有原生桥时走原生，不碰浏览器合成',
      nativeSaid.join('|') === '起立' && webSaid.length === 0,
      JSON.stringify({ n: nativeSaid, w: webSaid }));

V = setup({ nativeThrows: true });
V.say('起立');
tick();
check('旧安装包没有这个桥方法时退回浏览器合成',
      webSaid.join('|') === '起立', JSON.stringify(webSaid));

V = setup({ nativeThrows: true });
V.say('起立');
tick();
V.say('趴下');
tick();
check('原生撞过一次就不再撞第二次',
      webSaid.join('|') === '起立|趴下', JSON.stringify(webSaid));

V = setup({ noNative: true });
V.say('起立');
tick();
check('网页版走浏览器合成', webSaid.join('|') === '起立', JSON.stringify(webSaid));

V = setup({ noNative: true, noWebSynth: true });
V.say('起立');
tick();
check('两个引擎都没有时安静收场，不抛错', true);
check('两个引擎都没有时如实报不可用', V.available() === false);

// ---------------------------------------------------------------------------
console.log('\n== 对讲期间闭嘴 ==');
// ---------------------------------------------------------------------------
// 平板喇叭念出来的话会被自己的麦克风采回去，传到狗那侧就是回声。

V = setup();
talkOn = true;
clickOn(fakeEl({}, '起立'));
tick();
check('按住说话时不出声', nativeSaid.length === 0, nativeSaid.join('|'));
talkOn = false;
clickOn(fakeEl({}, '趴下'));
tick();
check('松开之后照常念', nativeSaid.join('|') === '趴下', nativeSaid.join('|'));

// ---------------------------------------------------------------------------
console.log('\n== 开关 ==');
// ---------------------------------------------------------------------------

storage = {};
V = setup();
check('默认是开着的', V.enabled() === true);
check('设置面板里的勾也是开着的', elements['set-voice'].checked === true);

elements['set-voice'].change(false);
tick();
clickOn(fakeEl({}, '起立'));
tick();
check('关掉之后一句都不念', nativeSaid.length === 0, nativeSaid.join('|'));
check('关掉的状态记在本机', storage['x30.voice'] === '0');

V = setup();
check('重开页面后还是关着', V.enabled() === false);

elements['set-voice'].change(true);
tick();
check('打开时念一句，当场验证平板真出得了声',
      nativeSaid.join('|') === '语音播报已开启', nativeSaid.join('|'));
check('打开的状态也记在本机', storage['x30.voice'] === '1');

// ---------------------------------------------------------------------------
console.log('\n== 设置面板那行提示要说清为什么不出声 ==');
// ---------------------------------------------------------------------------
// 「按了没声音」现场只会被当成 App 坏了。到底是引擎还没起来、还是平板上压根没装
// 中文语音包，只有这一行能说明白。

storage = {};
V = setup({ nativeStatus: 'none' });
check('没装中文语音包时说到语音包',
      elements['set-voice-hint'].textContent.indexOf('语音包') >= 0,
      elements['set-voice-hint'].textContent);

V = setup({ nativeStatus: 'init' });
check('引擎还在启动时说清第一句会晚',
      elements['set-voice-hint'].textContent.indexOf('启动') >= 0,
      elements['set-voice-hint'].textContent);

V = setup({ noNative: true });
check('网页版说明是浏览器在念',
      elements['set-voice-hint'].textContent.indexOf('浏览器') >= 0,
      elements['set-voice-hint'].textContent);

V = setup({ noNative: true, noWebSynth: true });
check('本机念不了时明说开着也没用',
      elements['set-voice-hint'].textContent.indexOf('不会出声') >= 0,
      elements['set-voice-hint'].textContent);

console.log('\n通过 ' + pass + '，失败 ' + fail);
process.exit(fail === 0 ? 0 : 1);
