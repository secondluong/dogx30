// 按键语音播报。
//
// 为什么要有：现场遥控时人盯的是狗，不是平板 —— 戴着手套、在太阳底下，按下去到底
// 进没进指令，全靠屏幕上那条横幅说，而横幅恰好在没人看的那块屏上。念一声出来，
// 手上按的和机器上发生的才对得起来。
//
// 三个不显然、但决定了本文件长这样的点：
//
// 一、安卓 WebView 里没有 speechSynthesis。Web Speech 从来没在 WebView 上实现过，
//     window.speechSynthesis 有时甚至是存在的，但 getVoices() 是空的、speak() 静默
//     不出声 —— 只测网页会以为做完了，装到平板上一句都听不到。所以 App 壳走原生
//     TextToSpeech（见 Tts.java），网页那侧才用浏览器自带的合成。
// 二、念的是**做成了什么**，不是**按了哪颗**。按钮点下去可能被拦（没控制权、网关
//     没连、步态切不动），这时候还念按钮上的字就是在骗人，而语音比屏幕更容易被当真。
//     所以同一拍里后来的话覆盖先前的：委托监听先按按钮上的字念，处理函数认为结果
//     不是那么回事就改口，最后只出一句。
// 三、一律短句，两到五个字。这不是图省事：按键回执要跟得上手，一句话念到第三秒时
//     人早按下一颗了，那时候念出来的内容反而误导。长话留给横幅。

(function () {
  'use strict';

  var STORE = 'x30.voice';

  // 后一句总比前一句要紧：连按两颗键时人要听的是后一颗做成了什么。所以不排队，
  // 一律打断重念（原生那侧是 QUEUE_FLUSH）。排队播会越积越晚，最后念的还是几秒前
  // 的动作，比不念更糟。
  var RATE = 1.15;

  var state = {
    on: true,
    pending: '',
    timer: null,
    native: null,   // null=还没试过，true=原生桥能念，false=没有桥或旧版安装包
  };

  function load() {
    try {
      state.on = window.localStorage.getItem(STORE) !== '0';
    } catch (e) {
      state.on = true;   // 隐私模式下读不到，按默认开
    }
  }

  // -------------------------------------------------------------------------
  // 两个后端
  // -------------------------------------------------------------------------

  // 旧版安装包没有 speak 这个桥方法，调下去会抛。撞过一次就记住，别每次再撞。
  // 不用 typeof 判：安卓 WebView 里 X30Native 的方法 typeof 常常不是 function
  // （app.js 里 hasNativeRadio 同样的坑），那样会误判成没有原生 TTS。
  function nativeSpeak(text) {
    if (state.native === false) return false;
    try {
      var ok = !!window.X30Native.speak(text);
      state.native = true;
      return ok;
    } catch (e) {
      state.native = false;
      return false;
    }
  }

  function nativeStatus() {
    try {
      return String(window.X30Native.ttsStatus() || '');
    } catch (e) {
      return '';
    }
  }

  function webSynth() {
    if (!window.speechSynthesis || !window.SpeechSynthesisUtterance) return null;
    return window.speechSynthesis;
  }

  function webSpeak(text) {
    var synth = webSynth();
    if (!synth) return;
    try {
      synth.cancel();
      var u = new window.SpeechSynthesisUtterance(text);
      u.lang = 'zh-CN';
      u.rate = RATE;
      synth.speak(u);
    } catch (e) { /* 合成器不可用就安静，不能因为念不出话影响操控 */ }
  }

  function available() {
    return state.native !== false || !!webSynth();
  }

  // -------------------------------------------------------------------------
  // 发声
  // -------------------------------------------------------------------------

  // 按住说话时不出声：平板喇叭念的这句会被自己的麦克风采回去，传到狗那侧就是回声。
  function talking() {
    try {
      return !!(window.X30Media && window.X30Media.talking && window.X30Media.talking());
    } catch (e) {
      return false;
    }
  }

  function enabled() {
    return state.on;
  }

  // 同一拍里只出最后一句。委托监听在捕获阶段先念按钮上的字，处理函数随后（冒泡阶段）
  // 若认为结果不同就再念一次，覆盖掉前一句 —— 按钮上写「起立」而实际被「没有控制权」
  // 拦下时，人听到的必须是后者。
  function say(text) {
    if (!state.on || !text) return;
    state.pending = String(text);
    if (state.timer) return;
    state.timer = window.setTimeout(flush, 0);
  }

  function flush() {
    state.timer = null;
    var text = state.pending;
    state.pending = '';
    if (!text || !state.on || talking()) return;
    if (nativeSpeak(text)) return;
    webSpeak(text);
  }

  function stop() {
    state.pending = '';
    if (state.timer) {
      window.clearTimeout(state.timer);
      state.timer = null;
    }
    try {
      window.X30Native.ttsStop();
    } catch (e) { /* 网页没有原生桥 */ }
    var synth = webSynth();
    if (!synth) return;
    try {
      synth.cancel();
    } catch (e) { /* 合成器不可用 */ }
  }

  // -------------------------------------------------------------------------
  // 委托监听
  // -------------------------------------------------------------------------

  // 不逐颗按钮挂：控制台上四十来颗按钮散在六个文件里，逐个挂必定漏，而且以后新加
  // 按钮的人不会想起来回这里补一行 —— 表现就是「别的键都念，这颗不念」。
  // 默认念按钮上的字；字不适合念（品牌名、纯数字档位）的用 data-say 覆盖，
  // data-say="" 表示这颗不念。
  function phraseOf(el) {
    var custom = el.getAttribute('data-say');
    if (custom !== null) return custom;
    // 折叠菜单那三颗按钮上是「当前档位 ›」，箭头不能念，所以用它们的 aria-label。
    var label = el.getAttribute('aria-label');
    if (label) return label;
    return (el.textContent || '').replace(/\s+/g, ' ').trim();
  }

  function onClick(e) {
    var t = e.target;
    if (!t || !t.closest) return;
    var el = t.closest('button, [data-say]');
    if (!el || el.disabled) return;
    say(phraseOf(el));
  }

  // -------------------------------------------------------------------------
  // 开关
  // -------------------------------------------------------------------------

  function setEnabled(on) {
    state.on = !!on;
    try {
      window.localStorage.setItem(STORE, state.on ? '1' : '0');
    } catch (e) { /* 隐私模式下记不住，当次有效 */ }
    var box = document.getElementById('set-voice');
    if (box && box.checked !== state.on) box.checked = state.on;
    if (!state.on) {
      stop();
      return;
    }
    // 开的时候念一句：这是唯一能当场验证「平板真的出得了声」的办法 ——
    // 音量键调到零、语音包没装，这些都要在上狗之前发现。
    say('语音播报已开启');
  }

  function paintHint() {
    var el = document.getElementById('set-voice-hint');
    if (!el) return;
    var st = nativeStatus();
    var text;
    if (st === 'ok') {
      text = '由平板的系统语音引擎播报。';
    } else if (st === 'init') {
      text = '平板的语音引擎正在启动，第一句可能晚一点。';
    } else if (st === 'none') {
      text = '平板上没有可用的中文语音：多半是没装语音包，去系统设置里装一个。';
    } else if (webSynth()) {
      text = '由浏览器自带的语音合成播报。';
    } else {
      text = '本机没有可用的语音合成，开着也不会出声。';
    }
    el.textContent = text;
  }

  // 引擎起来要一秒左右，面板打开那一刻的结论才是准的，所以每次打开都重问一遍。
  function onSettingsOpen() {
    var box = document.getElementById('set-voice');
    if (box) box.checked = state.on;
    paintHint();
  }

  function initVoice() {
    load();
    var box = document.getElementById('set-voice');
    if (box) {
      box.checked = state.on;
      box.addEventListener('change', function () {
        setEnabled(box.checked);
      });
    }
    paintHint();
    document.addEventListener('click', onClick, true);
  }

  window.X30Voice = {
    initVoice: initVoice,
    say: say,
    stop: stop,
    enabled: enabled,
    setEnabled: setEnabled,
    available: available,
    onSettingsOpen: onSettingsOpen,
  };
})();
