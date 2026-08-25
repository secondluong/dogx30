// 控制台静态一致性检查：node tools/web_ui_test.js
//
// 为什么需要它：设置面板曾经**关不掉，而且一打开页面就自己挡在屏幕中间**。
// 代码是对的（close() 确实加了 .hidden），坏在 CSS 优先级上：
//
//   .hidden   { display: none; }   /* 第 109 行 */
//   .set-mask { display: flex; }   /* 第 376 行 */
//
// 两边都只有一个类，优先级持平，于是「谁写在后面谁赢」—— 而组件规则总是往
// 文件尾部加。所有 JS 测试都过了，端到端测试也过了，因为没有一个测试渲染 CSS。
//
// 这一类 bug 的共同点是：JS 读起来完全正确，人在浏览器里才发现点了没反应。
// 所以这里不测「.hidden 有没有写 !important」（那是照着修法抄答案，换成把
// .hidden 挪到文件末尾也同样正确），而是照 CSS 层叠规则真的算一遍：
// 对每个「本来就该能被隐藏」的元素，.hidden 到底赢不赢。
//
// 顺带查两件同样只有在浏览器里才暴露的事：JS 要找的 id 在 HTML 里是否存在，
// 以及设置面板的字段是否与网关认的键对得上。

'use strict';

var fs = require('fs');
var path = require('path');

var WEB = path.join(__dirname, '..', 'web');
var pass = 0, fail = 0;

function check(name, ok, detail) {
  if (ok) { pass++; console.log('  [OK]   ' + name); }
  else { fail++; console.log('  [FAIL] ' + name + (detail ? '  ' + detail : '')); }
}

function read(name) {
  return fs.readFileSync(path.join(WEB, name), 'utf8');
}

// --- CSS 解析 ---------------------------------------------------------------
// 只需要「选择器 / 声明 / 出现顺序」，不做完整 CSS 解析。@media 里的规则会被
// 它的外层花括号挡住，这里刻意不处理：控制台的显隐规则都在顶层。

function parseCss(text) {
  var clean = text.replace(/\/\*[\s\S]*?\*\//g, '');
  var rules = [];
  var re = /([^{}]+)\{([^{}]*)\}/g;
  var m, order = 0;
  while ((m = re.exec(clean)) !== null) {
    var body = m[2];
    m[1].split(',').forEach(function (sel) {
      sel = sel.trim();
      if (sel && sel.charAt(0) !== '@') {
        rules.push({ sel: sel, body: body, order: order++ });
      }
    });
  }
  return rules;
}

// (id, class, element) 三元组。够用了：控制台里没有 :not() 之类会影响计算的写法。
function specificity(sel) {
  var s = sel.replace(/::?[a-z-]+(\([^)]*\))?/g, ' ');
  return {
    id: (s.match(/#[\w-]+/g) || []).length,
    cls: (s.match(/[.\[][\w-]+/g) || []).length,
    el: (s.replace(/[#.\[][\w-]+\]?/g, ' ').match(/[a-z]+/gi) || []).length,
  };
}

function cmpSpec(a, b) {
  if (a.id !== b.id) return a.id - b.id;
  if (a.cls !== b.cls) return a.cls - b.cls;
  return a.el - b.el;
}

function declOf(body, prop) {
  var re = new RegExp('(?:^|;)\\s*' + prop + '\\s*:([^;]*)', 'g');
  var m, last = null;
  while ((m = re.exec(body)) !== null) last = m[1];
  if (last === null) return null;
  return { value: last.trim(), important: /!important/i.test(last) };
}

// sel 这个选择器，是否只用到 have 里有的类/id（即能匹配上这个元素）。
// 带后代组合符的规则一律算「可能匹配」——静态判断不了祖先，宁可多报。
function selectorMatches(sel, have) {
  if (/[\s>+~]/.test(sel.trim())) return true;
  var parts = sel.trim().replace(/::?[a-z-]+(\([^)]*\))?/g, '').match(/[#.][\w-]+/g);
  if (!parts) return false;
  return parts.every(function (p) { return have.indexOf(p) !== -1; });
}

// --- HTML 里的元素 ----------------------------------------------------------

function parseElements(html) {
  var out = [];
  var re = /<([a-z][\w-]*)\b([^>]*)>/gi;
  var m;
  while ((m = re.exec(html)) !== null) {
    var attrs = m[2];
    var id = (attrs.match(/\bid\s*=\s*"([^"]*)"/) || [])[1] || '';
    var cls = (attrs.match(/\bclass\s*=\s*"([^"]*)"/) || [])[1] || '';
    out.push({
      tag: m[1], id: id,
      classes: cls.split(/\s+/).filter(Boolean),
      line: html.slice(0, m.index).split('\n').length,
    });
  }
  return out;
}

var css = parseCss(read('style.css'));
var html = read('index.html');
var els = parseElements(html);
var js = ['app.js', 'settings.js', 'gamepad.js'].map(function (f) {
  return { name: f, text: read(f) };
});
var jsAll = js.map(function (f) { return f.text; }).join('\n');

// ---------------------------------------------------------------------------
console.log('\n== 旧 WebView 也要能解析 ==');
// ---------------------------------------------------------------------------
// G20 这类工业平板的 WebView 常年不更新。踩过的坑：app.js 里一处无绑定 catch
// （Chrome 66 起才有）让整个文件解析失败，于是界面停在 HTML 静态默认值，
// 屏幕按键一个都不发指令，而实体键走 Java 照样能动 —— 极难往语法上想。
// 这些写法在 Node 里全都合法，所以只能按文本查。
var LEGACY_TRAPS = [
  { re: /catch\s*\{/, name: '无绑定 catch（Chrome 66+）' },
  { re: /\?\./, name: '可选链 ?.（Chrome 80+）' },
  { re: /\?\?/, name: '空值合并 ??（Chrome 80+）' },
  { re: /\.replaceAll\(/, name: 'String.replaceAll（Chrome 85+）' },
  { re: /\.flatMap\(|\.flat\(/, name: 'Array.flat/flatMap（Chrome 69+）' },
  { re: /\.padStart\(|\.padEnd\(/, name: 'String.padStart（Chrome 57+）' },
  { re: /Object\.fromEntries/, name: 'Object.fromEntries（Chrome 73+）' },
  { re: /globalThis/, name: 'globalThis（Chrome 71+）' },
];
var webJsFiles = fs.readdirSync(WEB).filter(function (f) {
  return /\.js$/.test(f);
});
webJsFiles.forEach(function (f) {
  var text = read(f).replace(/\/\/[^\n]*/g, '').replace(/\/\*[\s\S]*?\*\//g, '');
  var hits = LEGACY_TRAPS.filter(function (t) { return t.re.test(text); })
                         .map(function (t) { return t.name; });
  check(f + ' 没有旧 WebView 不认的语法', hits.length === 0, hits.join('，'));
});

// ---------------------------------------------------------------------------
console.log('\n== 模块导出的名字必须真的存在 ==');
// ---------------------------------------------------------------------------
// media.js 曾经导出一个从没定义过的 onMediaPlan。那一行是文件最后一句，于是
// window.X30Media 整个赋值失败，视频、对讲、切布局全部静默失效，控制台只留一句
// ReferenceError。Node 里 require 不到这些文件，只能按文本核对。
webJsFiles.forEach(function (f) {
  var text = read(f);
  var m = /window\.(X30\w+)\s*=\s*\{([^}]*)\}/.exec(text);
  if (!m) return;
  var missing = m[2].split(',').map(function (s) {
    return s.split(':')[0].trim();
  }).filter(Boolean).filter(function (name) {
    var def = new RegExp('(?:function|const|let|var)\\s+' + name + '\\b');
    return !def.test(text);
  });
  check(f + ' 导出的 ' + m[1] + ' 成员都有定义', missing.length === 0,
        missing.join('，'));
});

// ---------------------------------------------------------------------------
console.log('\n== 各 js 顶层的名字不能互相撞车 ==');
// ---------------------------------------------------------------------------
// index.html 里这几个 <script> 不是模块，顶层声明全都落在同一个全局作用域。
// media.js 的 function isAppShell 和 app.js 的 const isAppShell 撞在一起，
// 后加载的 app.js 整份报 SyntaxError —— 界面于是停在 HTML 静态默认值，屏幕按键
// 全哑，而实体键走 Java 照样能动。单看任一文件都毫无破绽，只有合起来才犯规。
var loadOrder = (read('index.html').match(/<script src="[^"?]+/g) || [])
  .map(function (s) { return s.replace(/^<script src="/, ''); });
check('index.html 的脚本都找得到', loadOrder.every(function (f) {
  return webJsFiles.indexOf(f) >= 0;
}), loadOrder.join(' '));

var owner = {};
var clashes = [];
loadOrder.forEach(function (f) {
  var seen = {};
  read(f).split('\n').forEach(function (line, i) {
    // 只认顶层：缩进了的就是函数或块里面的，各自独立作用域。
    var m = /^(?:function|const|let|var|class)\s+([A-Za-z_$][\w$]*)/.exec(line);
    if (!m || seen[m[1]]) return;
    seen[m[1]] = true;
    var where = f + ':' + (i + 1);
    if (owner[m[1]]) clashes.push(m[1] + '（' + owner[m[1]] + ' 与 ' + where + '）');
    else owner[m[1]] = where;
  });
});
check('没有跨文件重名的顶层声明', clashes.length === 0, clashes.join('；'));

// 上面按行文本查，漏得掉写法特别的声明。再让 V8 真的解析一遍：按加载顺序拼成
// 一份（只解析不执行），重名、括号不配对这类早期错误都会在这里现原形。
var catErr = null;
try {
  new (require('vm').Script)(loadOrder.map(read).join('\n;\n'));
} catch (e) {
  catErr = e.message;
}
check('六个脚本合起来能被解析', catErr === null, catErr || '');

// ---------------------------------------------------------------------------
console.log('\n== .hidden 必须真的能隐藏 ==');
// ---------------------------------------------------------------------------

var hiddenRule = null;
css.forEach(function (r) {
  if (r.sel === '.hidden' && declOf(r.body, 'display')) hiddenRule = r;
});

check('style.css 里有 .hidden 规则', !!hiddenRule);

if (hiddenRule) {
  var hd = declOf(hiddenRule.body, 'display');
  check('.hidden 声明的是 display: none', hd.value.replace(/!important/i, '').trim() === 'none', hd.value);

  // 哪些元素本来就该能被隐藏：HTML 里写了 hidden 的，加上 JS 会去 toggle 的。
  var hideable = {};
  els.forEach(function (el) {
    if (el.classes.indexOf('hidden') !== -1) {
      hideable[el.id || el.classes.join('.')] = el;
    }
  });
  var re = /\$\('([\w-]+)'\)\.classList\.\w+\('hidden'/g;
  var m;
  while ((m = re.exec(jsAll)) !== null) {
    els.forEach(function (el) { if (el.id === m[1]) hideable[el.id] = el; });
  }
  // root / noteEl 这两个是先存进变量再用的，正则抓不到，显式补上 ——
  // 关不掉的那个 set-mask 恰好就是 root。
  ['settings', 'set-note'].forEach(function (id) {
    els.forEach(function (el) { if (el.id === id) hideable[id] = el; });
  });

  var names = Object.keys(hideable);
  check('找到了需要检查的可隐藏元素', names.length > 0, '共 ' + names.length + ' 个');

  var hSpec = specificity('.hidden');
  names.forEach(function (name) {
    var el = hideable[name];
    var have = el.classes.map(function (c) { return '.' + c; });
    if (el.id) have.push('#' + el.id);

    var beaten = [];
    css.forEach(function (r) {
      if (r === hiddenRule) return;
      var d = declOf(r.body, 'display');
      if (!d) return;
      if (d.value.replace(/!important/i, '').trim() === 'none') return;  // 也在隐藏，不冲突
      if (!selectorMatches(r.sel, have)) return;

      // 真正的层叠规则：!important 优先，其次比优先级，最后比谁在后面。
      var hiddenWins;
      if (hd.important !== d.important) hiddenWins = hd.important;
      else {
        var c = cmpSpec(hSpec, specificity(r.sel));
        hiddenWins = c !== 0 ? c > 0 : hiddenRule.order > r.order;
      }
      if (!hiddenWins) beaten.push(r.sel + ' { display: ' + d.value.trim() + ' }');
    });

    check('加上 .hidden 之后 ' + (el.id ? '#' + el.id : name) + ' 确实会消失',
          beaten.length === 0,
          beaten.length ? '被这条盖掉了：' + beaten.join('，') : '');
  });

  // 面板一打开页面就该是收着的。上面那个 bug 的另一半症状就是它自己弹出来。
  var panel = null;
  els.forEach(function (el) { if (el.id === 'settings') panel = el; });
  check('设置面板在 HTML 里初始带 hidden',
        !!panel && panel.classes.indexOf('hidden') !== -1,
        panel ? panel.classes.join(' ') : '没找到 #settings');
}

// ---------------------------------------------------------------------------
console.log('\n== JS 要找的 id 在 HTML 里必须存在 ==');
// ---------------------------------------------------------------------------
// 拼错一个 id 的表现是「点了没反应」，而且控制台里只有一条 null 异常，
// 在平板上根本看不到。

var htmlIds = {};
els.forEach(function (el) { if (el.id) htmlIds[el.id] = true; });

// 设置面板的输入框是 JS 按字段名生成的，不在 HTML 里，得排除掉。
var generated = {};
var secRe = /key:\s*'([\w]+)'/g;
var mm;
while ((mm = secRe.exec(read('settings.js'))) !== null) {
  generated['set-' + mm[1].replace(/_/g, '-')] = true;
}

js.forEach(function (f) {
  var missing = [];
  var re2 = /\$\('([\w-]+)'\)/g, m2;
  var seen = {};
  while ((m2 = re2.exec(f.text)) !== null) {
    var id = m2[1];
    if (seen[id]) continue;
    seen[id] = true;
    if (!htmlIds[id] && !generated[id]) missing.push(id);
  }
  check(f.name + ' 引用的 id 都在 index.html 里', missing.length === 0,
        missing.length ? '找不到：' + missing.join(', ') : '');
});

check('顶栏有背景按钮', /id="btn-view">背景<\/button>/.test(html));
check('左下只有一个姿态按钮', /id="btn-stand"/.test(html) && html.indexOf('btn-unload') === -1);
check('点标题打开设置', /id="btn-settings"[^>]*>\s*X30 遥控台/.test(html));
check('设置面板在 CSS 到来前就藏着',
      /<style>\s*\.hidden\s*\{\s*display\s*:\s*none\s*!important/.test(html));
check('没有单独的订阅点云按钮', html.indexOf('订阅点云') === -1 && !htmlIds['btn-cloud']);
check('点云画面有设置按钮', !!htmlIds['btn-cloud-settings']);
check('点云菜单有显控按钮', !!htmlIds['btn-cloud-vis']);
check('点云菜单初始收着',
      /id="cloud-ctl"[^>]*\bhidden\b/.test(html) ||
      /class="[^"]*\bhidden\b[^"]*"[^>]*id="cloud-ctl"/.test(html));

// 网页和 App 两套 HUD，改一处容易把另一处一起藏掉。
var styleText = read('style.css');
var appJs = read('app.js');
check('急停后同一按钮转卸力',
      /emergencyLocked/.test(appJs) && /textContent = '卸力'/.test(appJs) &&
      /name = 'unload'/.test(appJs));
check('网页仍有截图按钮', /data-capture="shot"/.test(html));
check('App 壳去掉截图录屏按钮',
      /html\.shell-app[\s\S]*?\.pane-actions/.test(styleText));
check('网页有指标按钮', !!htmlIds['btn-telem']);
check('网页有摇杆按钮', !!htmlIds['btn-sticks']);
check('网页有气体按钮', !!htmlIds['btn-gas']);
check('网页没有手柄按钮', !htmlIds['btn-gp'] && html.indexOf('>手柄<') === -1);
check('App 壳藏掉网页摇杆和指标按钮',
      /html\.shell-app[\s\S]*?\.hud-sticks/.test(styleText) &&
      /html\.shell-app[\s\S]*?\.hud-telem-btn/.test(styleText) &&
      /html\.shell-app[\s\S]*?\.hud-gas-btn/.test(styleText));
check('趴下不锁死网页摇杆',
      !/\.dog-prone\s+\.hud-sticks/.test(styleText) &&
      /id="btn-sticks"/.test(html) &&
      /id="hud-sticks"/.test(html));
check('控布控球时摇杆不看狗站没站',
      /usable = ptz/.test(appJs) &&
      /\? true/.test(appJs) &&
      !/usable = app\.hasControl && app\.alive && controlChannel\(\) !== null/.test(appJs));
check('断网时不把人堵在控制权上',
      /function linkOpen/.test(appJs) &&
      /function radioHint/.test(appJs) &&
      /radioOnly\(\) \|\| !linkOpen\(\)/.test(appJs));
check('App 顶栏有 2.4G/MESH 切换',
      !!htmlIds['btn-radio'] &&
      /function setRadioPath/.test(appJs) &&
      /function adoptRadioPath/.test(appJs) &&
      /html\.shell-app #btn-radio/.test(styleText) &&
      /html\.shell-app #btn-control/.test(styleText));
check('切 2.4G 不经网关下发',
      /radioPath === 'radio'/.test(appJs) &&
      /t === 'claim' \|\| t === 'vel'/.test(appJs) &&
      /已切到 2.4G/.test(appJs) &&
      /nativeRadioCmd/.test(appJs) &&
      /radioCmdFromEl/.test(appJs) &&
      /applyRadioPose/.test(appJs) &&
      /function meshWsUrl/.test(appJs) &&
      /getRadioPath/.test(appJs));
// 操控和画面是两条链路：指令走 2.4G 数传，视频/遥测/点云走网关 WebSocket。
// 曾经切 2.4G 就整个不连网关，于是狗能动但一路画面都没有。切档只准改指令走哪边。
check('切 2.4G 仍连网关，画面不跟着断',
      // connect() 里不许再为 2.4G 提前 return
      !/if \(radioDirect\(\)\) \{\s*applyRadioPath/.test(appJs) &&
      // 掉线重连不看当前档位，否则 2.4G 下网关一断就再也回不来
      !/if \(app\.radioPath === 'radio'\) return;\s*setTimeout\(connect/.test(appJs) &&
      // 切档不许顺手把 ws 关掉
      !/app\.ws = null;/.test(appJs) &&
      /if \(changed\) connect\(\);/.test(appJs));
// 顶栏原来挂一条「2.4G通 网关通 ok/fail rx…」的状态串。它当初是为了把「狗能动
// 但没画面」拆成两头看 —— 那时 2.4G 下的画面还得靠网关。改成原生直拉 RTSP 之后
// 2.4G 根本不经过网关，这条串就只剩占地方和费神了。
// 现在通没通只看按钮颜色：字是档位，绿=这条路通，黄=不通；不通的原因用黄条讲一次。
check('链路通没通看按钮颜色，不在顶栏挂状态串',
      !/'网关通' : '网关断'/.test(appJs) &&
      !/st\.ready \? '2\.4G/.test(appJs) &&
      !/function radioStatusLine/.test(appJs) &&
      /function radioLinkUp/.test(appJs) &&
      /btn\.classList\.toggle\('link-up', up\)/.test(appJs) &&
      /btn\.classList\.toggle\('link-down', !up\)/.test(appJs) &&
      /const up = radio \? radioLinkUp\(st0\) : linkOpen\(\)/.test(appJs) &&
      /#btn-radio\.link-up/.test(styleText) &&
      /#btn-radio\.link-down/.test(styleText) &&
      /html\.radio-24 #chip-link \{ display: none/.test(styleText) &&
      !/radio-stat/.test(styleText));
check('原生切 2.4G 能找到 window.app',
      /window\.app = app/.test(appJs) &&
      /function syncNativeRadioPath/.test(appJs) &&
      /function fireRadioFromEl/.test(appJs) &&
      /radioDirect\(\) && hasNativeRadio/.test(appJs) &&
      /X30Native\.toggleRadioPath/.test(appJs));
// 切档按钮就是顶栏里那一个普通网页按钮。曾经改成原生浮层压在画面左上角，
// 是因为当时 app.js 整份没跑、网页按钮点了没用；根因修掉后就该放回顶栏，
// 那里不会遮挡画面，也不用再给它在顶栏占位。
check('App 壳的 2.4G 按钮就在顶栏里',
      /html\.shell-app #btn-radio \{[^}]*display:\s*inline-block/.test(styleText) &&
      !/radio-slot/.test(html) &&
      !/radio-slot/.test(styleText));
var radioJava = fs.readFileSync(
  path.join(__dirname, '..', 'android-app', 'app', 'src', 'main', 'java',
            'com', 'dogx30', 'control', 'RadioLink.java'), 'utf8');
var radioBridge = fs.readFileSync(
  path.join(__dirname, '..', 'android-app', 'app', 'src', 'main', 'java',
            'com', 'dogx30', 'control', 'ControlActivity.java'), 'utf8');
// 姿态交接是网页、原生、网关三处一起改才成立的，少一处就是「切档姿态又变了」。
var motionHpp = fs.readFileSync(
  path.join(__dirname, '..', 'rk3588', 'include', 'x30', 'motion_client.hpp'), 'utf8');
var serviceCpp = fs.readFileSync(
  path.join(__dirname, '..', 'rk3588', 'src', 'robot_service.cpp'), 'utf8');
var motionCpp = fs.readFileSync(
  path.join(__dirname, '..', 'rk3588', 'src', 'motion_client.cpp'), 'utf8');
check('2.4G RadioLink 含起立趴下行走指令',
      /0x21010223/.test(radioJava) &&
      /0x21010222/.test(radioJava) &&
      /0x21010201/.test(radioJava) &&
      /bindSocket/.test(radioJava) &&
      /192, 168, 144/.test(radioJava) &&
      /createUDPPipeline/.test(radioJava) &&
      /KeyRCRFEnable/.test(radioJava) &&
      /osIfaces/.test(radioJava) &&
      /bindToDevice/.test(radioJava) &&
      /SO_BINDTODEVICE/.test(radioJava) &&
      /HandlerThread/.test(radioJava) &&
      /radio-udp/.test(radioJava) &&
      /drainRx/.test(radioJava) &&
      /buttonsPrimed/.test(radioJava) &&
      /rcLive/.test(radioJava) &&
      /LOCAL_PORT/.test(radioJava) &&
      /192\.168\.1\.103/.test(radioJava) &&
      !/192\.168\.144\.10/.test(radioJava) &&
      /onRcReady/.test(radioJava) &&
      !/maybeStep/.test(radioJava) &&
      !/createG12G20Pipeline/.test(radioJava) &&
      /void command\(String name\)/.test(radioJava) &&
      /void standUp\(\)/.test(radioJava) &&
      /STAND_AFTER_UNLOAD_MS/.test(radioJava) &&
      /radioStanding/.test(radioBridge) &&
      /radioLinkOk/.test(radioBridge) &&
      /radioStatus/.test(radioBridge));
// 狗一直在往遥控器发遥测（0x1009 那一包头后第一个字节就是 basic_state），
// 以前 drainRx 只数包不看内容，姿态全靠「我发过什么」猜。别的遥控器动过狗、
// 或者刚从 MESH 切回来，猜的和实际就是两回事。
check('2.4G 下读狗自己报的姿态，不靠本地猜',
      /TELEM_MOTION = 0x1009/.test(radioJava) &&
      /void readTelem\(byte\[\] b, int len\)/.test(radioJava) &&
      /readTelem\(buf, p\.getLength\(\)\)/.test(radioJava) &&
      /telemFresh\(\)/.test(radioJava) &&
      /o\.put\("basic"/.test(radioJava) &&
      /o\.put\("gait"/.test(radioJava));
// 姿态要写进 MESH 那侧同一个 app.basicState：两条链路读同一份真相，
// 切档时才对得上。以前 2.4G 只改 rlStanding，切回去还留着旧读数，于是总显示站立。
check('两条链路共用一份姿态，切档不走散',
      /function syncRadioStanding/.test(appJs) &&
      /st\.basic/.test(appJs) &&
      /app\.basicState = basic/.test(appJs) &&
      /app\.emergencyLocked = locked/.test(appJs) &&
      /STATE_TORQUE_STANDING/.test(appJs));
// 我们起立发的是 0x21010223（RL 起立，原厂手柄同一条），运动主机在这之后仍报
// basic_state=0，而原厂手柄此时就能走 —— 网关那侧的 AxisCommandsApply 早有这条，
// 2.4G 这侧漏了，于是非要先点力控、起步。两侧规则必须一致。
check('起立后就能走，不用先点力控起步',
      /private boolean axesApply\(\)/.test(radioJava) &&
      /if \(axesApply\(\)\) sendAxes/.test(radioJava) &&
      /ST_TORQUE_STANDING \|\| telemState == ST_STEPPING/.test(radioJava) &&
      /AXIS_AFTER_STAND_MS/.test(radioJava) &&
      !/if \(torqued \|\| stepping\) \{/.test(radioJava));
// 起立中 / 坐下中 / 急停仍然不发轴：那几个状态下轴没有文档定义，
// 实测会把原厂柔和的起身趴下掐成猛起猛趴。
check('过渡和急停期间不发轴',
      /ST_SIT_TO_STAND/.test(radioJava) &&
      /ST_STAND_TO_SIT/.test(radioJava) &&
      /ST_EMERGENCY/.test(radioJava));
// 上电时通道常是全 0，(0-1500)/500 会被读成满量程后退。以前要先点起步才发轴，
// 现在起立后就发，这一脚会直接踹出去。
check('通道没活起来之前不当摇杆量用',
      /ch\.length > 0 && rcLive\(ch\)/.test(radioJava));
// 桥接方法一直在，实现却是个空壳：2.4G 下推屏幕摇杆没有任何反应。
// 姿态遥测是狗单播回来的，只发给 network.toml 里登记过的地址。收不到时姿态只能猜，
// 而「为什么猜错」这件事没有别的办法看出来，所以设置里要能查到本机地址和收没收到。
check('设置里查得到姿态遥测通不通',
      !!htmlIds['set-app-radio-stat'] &&
      /set-app-radio-stat/.test(read('settings.js')) &&
      /network\.toml/.test(read('settings.js')) &&
      /43897/.test(read('settings.js')) &&
      /basic < 0 && app\.basicState !== 0/.test(appJs));
// 「站没站」两条链路各记一份：2.4G 的起立不经过网关，网关的起立 RadioLink 也不知道，
// 而狗 RL 起立后遥测仍报坐下，谁都认不出来。所以切档那一刻必须交接，否则切一次
// 姿态就变回趴着，而且轴被接手那侧吞掉，狗站着却推不动。
check('切档时把姿态交接给接手的一侧',
      /function handoffPose/.test(appJs) &&
      /pushPoseToRadio/.test(appJs) &&
      /app\.poseHandoff = upright/.test(appJs) &&
      /claim\.standing = app\.poseHandoff/.test(appJs) &&
      /function checkPoseHint/.test(appJs) &&
      /app\.claimMsg/.test(read('gamepad.js')) &&
      /radioAdoptPose/.test(radioBridge) &&
      /void adoptPosture\(boolean up\)/.test(radioJava) &&
      /onRadio\(\(\) -> adoptOnRadio\(up\)\)/.test(radioJava) &&
      /void AdoptPosture\(bool standing\)/.test(motionHpp) &&
      /msg\.Has\("standing"\)/.test(serviceCpp));
// 交接不能发一次就算完：那条 claim 可能没被授权，网关也可能是旧版不认这个键。
// 认下来之前界面按自己知道的显示，否则切回 MESH 左下角又变成「起立」。
check('网关认下来之前按自己知道的显示',
      /app\.rlStanding = app\.poseHandoff !== null \? app\.poseHandoff/.test(appJs) &&
      /if \(obj\.t === 'cmd' && POSE_CMDS\[obj\.name\]\) app\.poseHandoff = null/.test(appJs) &&
      !/app\.poseHandoff = null;\s*\n\s*poseHintVal/.test(appJs));
// 猜错的代价是按「趴下」时狗反而站起来，比多按一次起立严重得多，所以
// 只在发过起立/趴下或收到遥测时才交接。
check('姿态没把握就不交接',
      /poseIsKnown\(\)/.test(radioJava) &&
      /o\.put\("poseKnown"/.test(radioJava) &&
      /st\.poseKnown \? isStandingUi\(\) : null/.test(appJs) &&
      /linkOpen\(\) && app\.alive \? isStandingUi\(\) : null/.test(appJs) &&
      /if \(granted && msg\.Has\("standing"\)\)/.test(serviceCpp));
// 步态/踏面/身高收起来之后看不出选了哪一档，展开一次才知道 —— 遥控时是负担。
check('菜单收起时就显示当前档位加箭头',
      !!htmlIds['acc-val-gait'] &&
      !!htmlIds['acc-val-stair'] &&
      !!htmlIds['acc-val-height'] &&
      /function paintPickers/.test(appJs) &&
      /paintPickers\(\);/.test(appJs) &&
      /app\.gaitPending/.test(appJs) &&
      /el\.dataset\.label/.test(appJs) &&
      /class="acc-arrow"[^>]*>›</.test(html) &&
      /\.acc-arrow \{/.test(styleText) &&
      /\.acc-pop \.btn\.sm\.active::after/.test(styleText));
// 三颗按钮上原来写的是菜单名（步态/上下楼踏面/身高），现在换成当前那一档。
// 名字只在还不知道选了什么时兜底，所以留在 data-label 里，不再硬写在按钮文字上。
check('按钮上不再重复菜单名',
      /data-label="步态"/.test(html) &&
      !/data-acc="gait">步态</.test(html) &&
      /aria-label="步态"/.test(html));
// 刚开机或刚切过来时我们什么都没点过，档位只能问狗。身高是单独一条报文，
// 不解它就只能显示「我点过的」，那在切档之后必然是错的。
check('2.4G 的档位也听狗自己报',
      /TELEM_HEIGHT = 0x11050F08/.test(radioJava) &&
      /telemHeightSeen/.test(radioJava) &&
      /o\.put\("height", telemHeight\)/.test(radioJava) &&
      /RADIO_GAIT_KEYS/.test(appJs) &&
      /function syncRadioPickers/.test(appJs) &&
      /syncRadioPickers\(radioSt\)/.test(appJs));
// 力控站立、踏步不是运动学上的必要动作，只是状态机的两级台阶。原厂 App 起立后
// 推杆直接走，我们却要操作员先点力控再点起步 —— 戴手套在阳光下容易点错，顺序也
// 没人记得。现在推杆时两条链路各自替他踩。
check('推杆就走，力控和起步不用人点',
      /void ArmForWalk\(\)/.test(motionHpp) &&
      /void MotionClient::ArmForWalk\(\)/.test(motionCpp) &&
      /ArmForWalk\(\);/.test(motionCpp) &&
      /kWalkIntent/.test(motionCpp) &&
      /private void armForWalk\(\)/.test(radioJava) &&
      /if \(axesPushed\(ax\)\) armForWalk\(\)/.test(radioJava));
// 台阶要一级一级踩：力控刚发就跟一条踏步，运动主机还在过渡里会丢掉后一条，
// 现场表现是推了半天狗只在原地力控站着。
check('两级台阶之间留出状态机迁移时间',
      /kArmGapMs/.test(motionCpp) &&
      /arm_next_/.test(motionCpp) &&
      /ARM_GAP_MS/.test(radioJava) &&
      /now < armNextAt/.test(radioJava) &&
      /onRadioDelayed\(stepTask, ARM_GAP_MS\)/.test(radioJava));
// 力控站立唯一的用处是用摇杆调身高/俯仰。人点过力控就不能替他起步，否则这个
// 功能没了；没点过时推杆一律当「要走」，哪怕遥测报的是力控站立。
check('人点了力控就不替他起步',
      /function noteWalkIntent/.test(appJs) &&
      /app\.torqueByUser = true/.test(appJs) &&
      /if \(app\.torqueByUser &&/.test(appJs) &&
      /if \(app\.basicState === STATE_TORQUE_STANDING\) return 'vel'/.test(appJs) &&
      /if \(emergency \|\| torqueByUser\) return;/.test(radioJava) &&
      /torqueByUser = true/.test(radioJava));
// 上下楼那三档是必选一项：狗还没进楼梯步态时也得亮着一个，否则操作员不知道上楼
// 会用哪种。「上楼用这个」（pick）与「狗真在这个步态」（active）分开记 ——
// 后者跟着遥测灭，前者不能灭。
check('上下楼三档必选其一',
      /class="btn sm pick" data-gait="stair"/.test(html) &&
      /function markStairPick/.test(appJs) &&
      /markStairPick\(b\.dataset\.gait\)/.test(appJs) &&
      /markStairPick\(key\)/.test(appJs) &&
      /#stair-row \[data-gait\]\.pick/.test(appJs) &&
      /data-short="多帧"/.test(html) &&
      /on\.dataset\.short \|\| on\.textContent/.test(appJs) &&
      /\.hud-menus \.hud-walk \{ flex-wrap: nowrap; \}/.test(styleText) &&
      /\.acc-pop \.btn\.sm\.pick \{/.test(styleText) &&
      /\.acc-pop \.btn\.sm\.pick::after/.test(styleText));

// 步态编码到按钮键的映射现在有两份：网关的 GaitKey() 和 2.4G 直连用的
// RADIO_GAIT_KEYS。走歪了的表现是同一只狗在两条链路上高亮不同的步态。
var protoHpp = fs.readFileSync(
  path.join(__dirname, '..', 'rk3588', 'include', 'x30', 'protocol.hpp'), 'utf8');
var gaitEnum = {};
((protoHpp.match(/enum class Gait[^{]*\{([\s\S]*?)\}/) || [])[1] || '')
  .split(',').forEach(function (line) {
    var m = line.match(/(k\w+)\s*=\s*(\d+)/);
    if (m) gaitEnum[m[1]] = m[2];
  });
var cppGaits = {};
serviceCpp.replace(/case Gait::(k\w+): return "(\w+)";/g, function (_, sym, key) {
  if (gaitEnum[sym] !== undefined) cppGaits[gaitEnum[sym]] = key;
  return '';
});
var jsGaits = {};
((appJs.match(/const RADIO_GAIT_KEYS = \{([\s\S]*?)\};/) || [])[1] || '')
  .replace(/(\d+):\s*'(\w+)'/g, function (_, num, key) {
    jsGaits[num] = key;
    return '';
  });
var gaitNums = Object.keys(cppGaits);
var mismatch = gaitNums.filter(function (n) { return cppGaits[n] !== jsGaits[n]; })
  .concat(Object.keys(jsGaits).filter(function (n) { return !cppGaits[n]; }));
check('两条链路对步态编码的理解一致',
      gaitNums.length >= 9 && mismatch.length === 0,
      gaitNums.length + ' 个，不一致: ' + mismatch.join(', '));
check('屏幕摇杆在 2.4G 下也能推',
      /void setScreenAxes\(float fwd, float lat, float turn\)/.test(radioJava) &&
      /scrAt = System\.currentTimeMillis\(\)/.test(radioJava) &&
      /SCREEN_AXIS_HOLD_MS/.test(radioJava) &&
      /radioVel/.test(radioBridge) &&
      /radioVel\(/.test(appJs));
// 版本从顶栏挪进设置面板：遥控时顶栏要盯别的东西。但不能不印 ——
// 装包漏拷 assets/web 时页面是旧的，除了对版本戳没有别的办法看出来。
check('版本不占顶栏，但在设置里查得到',
      !htmlIds['chip-ver'] &&
      !/chip-ver/.test(html) &&
      !!htmlIds['set-app-ver'] &&
      /\$\('set-app-ver'\)/.test(appJs) &&
      /const WEB_BUILD/.test(appJs) &&
      /function paintVerChip/.test(appJs) &&
      /getAppVersion/.test(appJs) &&
      /getAppVersion/.test(radioBridge));
check('网页启动不受挂按钮那段成败影响',
      /function bootstrap\(\)/.test(appJs) &&
      /setTimeout\(bootstrap, 0\)/.test(appJs) &&
      /if \(booted\) return;/.test(appJs));
var radioLayout = fs.readFileSync(
  path.join(__dirname, '..', 'android-app', 'app', 'src', 'main', 'res',
            'layout', 'activity_control.xml'), 'utf8');
// 那层左上角的版本 / JS 报错 / 2.4G 状态浮层是查「屏幕按键全哑」时的临时手段，
// 根因（顶层重名让 app.js 整份没跑）修掉后就撤了 —— 它遮画面且一直在抢注意力。
// 版本仍然看得到，在设置面板 set-app-ver 那一处。诊断要再上，请另开一个入口，
// 别压回画面上。
check('App 壳不再有原生诊断浮层',
      !/setWebChromeClient/.test(radioBridge) &&
      !/probeJs/.test(radioBridge) &&
      !/jsErrs/.test(radioBridge) &&
      !/packagedWebBuild/.test(radioBridge) &&
      !/btn_radio_path/.test(radioLayout) &&
      !/txt_radio_stat/.test(radioLayout));
check('链路只跟顶栏 MESH/2.4G 按钮',
      /function setRadioPath/.test(appJs) &&
      /function adoptRadioPath/.test(appJs) &&
      /function meshWsUrl/.test(appJs) &&
      /X30Native\.setRadioPath/.test(appJs) &&
      !/nativeCall/.test(appJs) &&
      !/onclick=/.test(html.match(/id="btn-radio"[\s\S]*?>/)[0]) &&
      /void toggleRadioPath\(/.test(radioBridge) &&
      /void pushRadioPathToWeb\(/.test(radioBridge) &&
      /saveRadioPath/.test(radioBridge) &&
      /android_asset\/web\/index\.html/.test(radioBridge) &&
      !/gatewayReachable/.test(radioBridge) &&
      !/已自动切到 2\.4G/.test(appJs));
check('网页不出现链路切换按钮',
      /#btn-radio \{[^}]*display:\s*none/.test(styleText));

// 2.4G 是遥控器与狗直连，网关不在链路上：拿不到媒体计划，也够不到 MediaMTX。
// 狗只给 RTSP，而 WebView 放不了 RTSP —— 所以这一路必须原生解码。
// 这几条断言盯的是最容易被后人「顺手简化」掉、且一简化就整路黑屏的地方。
var dogCamJs = read('dogcam24.js');
var nativeVideo = fs.readFileSync(
  path.join(__dirname, '..', 'android-app', 'app', 'src', 'main', 'java',
            'com', 'dogx30', 'control', 'NativeVideo.java'), 'utf8');
var appGradle = fs.readFileSync(
  path.join(__dirname, '..', 'android-app', 'app', 'build.gradle'), 'utf8');
// 机身相机这一路归谁拉是二选一。平板同时连着 WiFi 时网关那一路也够得到，
// 于是同一只相机被拉两遍：白占本来就窄的 2.4G，两边还抢同一块占位图 ——
// 现场看到的就是「有时候从 RTSP 拉、有时候不」。
var mediaText = read('media.js');
check('机身相机不会被原生和网关同时拉两遍',
      /function nativeOwnsDogCam/.test(mediaText) &&
      /native-video-on/.test(mediaText) &&
      /if \(own && main === 'dog_cam'\) return \[\];/.test(mediaText) &&
      /tile\.id === 'dog_cam' && nativeOwnsDogCam\(\)/.test(mediaText) &&
      /resync/.test(mediaText) &&
      /function handOver/.test(dogCamJs) &&
      /X30Media\.resync/.test(dogCamJs));
check('2.4G 的机身相机走原生 RTSP',
      /rtsp:\/\/192\.168\.1\.105:8554/.test(dogCamJs) &&
      /videoStart/.test(dogCamJs) &&
      /videoStart/.test(radioBridge) &&
      /videoRect/.test(radioBridge) &&
      /RtspMediaSource/.test(nativeVideo) &&
      /media3-exoplayer-rtsp/.test(appGradle));
// 默认先试 UDP、超时再退 TCP：既白等一个超时，还要额外操心那几个临时 UDP 端口
// 有没有绑对网卡。走 TCP 时 RTSP 与 RTP 复用同一条连接，绑一条就全在 2.4G 上。
// 但不能死磕 TCP：个别 RTSP 服务只认 UDP。失败后换一种再试，报错里带上是哪种。
check('拉流先走 RTP over TCP，失败了换一种再试',
      /setForceUseRtpTcp\(forceTcp\)/.test(nativeVideo) &&
      /forceTcp = true/.test(nativeVideo) &&
      /forceTcp = !forceTcp/.test(nativeVideo) &&
      /"TCP: " : "UDP: "/.test(nativeVideo));
// ar_net0 是虚口，ConnectivityManager 常看不见它，普通 socket 会被安卓按默认网络
// 路由出去，直接 Network unreachable —— 和 RadioLink 里 UDP 踩过的是同一个坑。
check('拉流的 socket 绑到 2.4G 那张网卡',
      /setSocketFactory/.test(nativeVideo) &&
      /extends SocketFactory/.test(nativeVideo) &&
      /bindToDevice/.test(nativeVideo) &&
      /airNetwork/.test(nativeVideo) &&
      /static void bindToDevice\(Socket/.test(radioJava) &&
      /synchronized String ifaceName\(\)/.test(radioJava));
// 原生画面垫在 WebView 底下。网页那层不透明，或者 PlayerView 排到 WebView 后面，
// 结果都是「明明在放却看不到」。
check('原生画面垫在网页底下且背景透出',
      /native_video/.test(radioLayout) &&
      radioLayout.indexOf('native_video') < radioLayout.indexOf('@+id/web') &&
      /setBackgroundColor\(Color\.TRANSPARENT\)/.test(radioBridge) &&
      /html\.native-video-on[\s\S]*?background:\s*transparent/.test(styleText) &&
      /native-video-on/.test(dogCamJs));
// 画面格子的位置只有网页知道（横幅、布局都会挪它），原生按它报的矩形摆。
check('画面矩形按设备像素报给原生',
      /devicePixelRatio/.test(dogCamJs) &&
      /videoRect\(r\.x, r\.y, r\.w, r\.h\)/.test(dogCamJs) &&
      /void setRect\(int x, int y, int w, int h\)/.test(nativeVideo) &&
      /X30DogCam\.onStageLayout\(\)/.test(appJs));
// 相机没起来、地址填错、网卡没绑上，报错完全不同。不写到占位图上就只能翻日志。
check('没画面时占位图说得出原因',
      /onVideoState/.test(nativeVideo) &&
      /X30DogCam&&X30DogCam\.onState/.test(radioBridge) &&
      /拉流失败/.test(dogCamJs) &&
      /RETRY_MS/.test(nativeVideo));
// 布控球挂在网关那侧的 192.168.10.0/24，2.4G 到不了。不说清楚就像设备坏了。
check('2.4G 下布控球如实标不可用',
      /布控球在网关那侧/.test(dogCamJs) &&
      /media-idle-ptz-vis/.test(dogCamJs));
// 相机地址不该写死在包里：现场换过相机或改过端口，不能为此重新编包。
check('机身相机地址能在设置里改',
      !!htmlIds['set-app-dogcam'] &&
      /X30DogCam\.url\(\)/.test(read('settings.js')) &&
      /X30DogCam\.setUrl/.test(read('settings.js')));
// ExoPlayer 默认起播前先攒 2.5 秒，那是点播的调法；直播流攒进去的每一毫秒都变成
// 永久延迟（按 1 倍速从起点往后放，吐不出来）。遥控宁可偶尔卡一下也不要慢一截。
// 音轨也要关：画面得跟音频时钟对齐，AudioTrack 那点缓冲就成了延迟下限，
// 而这一路只是拿来看的（网页侧一直 muted，2.4G 下也没有对讲）。
check('拉流按低延迟配，不用点播那套缓冲',
      /setLoadControl/.test(nativeVideo) &&
      /setBufferDurationsMs/.test(nativeVideo) &&
      /setPrioritizeTimeOverSizeThresholds\(true\)/.test(nativeVideo) &&
      /BUFFER_MIN_MS = 200/.test(nativeVideo) &&
      /setTrackTypeDisabled\(C\.TRACK_TYPE_AUDIO, true\)/.test(nativeVideo));
// 光调小缓冲不够：链路抖一下就攒出一段，之后一直背着走。
check('攒出来的延迟会被追掉',
      /function trimLatency|private void trimLatency/.test(nativeVideo) &&
      /getTotalBufferedDuration/.test(nativeVideo) &&
      /setPlaybackSpeed/.test(nativeVideo) &&
      /CATCHUP_SPEED/.test(nativeVideo) &&
      /RESYNC_MS/.test(nativeVideo));
// 本机缓冲接近 0 而画面仍然慢，说明慢在上游，调客户端没用。要能分清。
check('设置里看得到本机缓冲了多少',
      !!htmlIds['set-app-dogcam-stat'] &&
      /function status\(\)/.test(dogCamJs) &&
      /X30DogCam\.status\(\)/.test(read('settings.js')) &&
      /bufferedMs/.test(radioBridge));
var mediaJs = read('media.js');
// 能力上报发在启动那一刻，那时 WebSocket 还没连上，send 会把它丢掉，
// 网关便一直按「只支持 H.264」下发计划。
check('链路连上后补发编码能力',
      /function onLinkOpen/.test(mediaJs) &&
      /X30Media\.onLinkOpen/.test(appJs));
check('桌面网页大屏也走布控球子码流',
      /function webH265Ok/.test(mediaJs) &&
      /media\.caps\.h265 = false/.test(mediaJs) &&
      /tile\.id !== 'dog_cam'/.test(mediaJs) &&
      /ptz_vis_sub/.test(mediaJs));
check('点大布控球时摇杆改控云台',
      /webStickTarget = 'ptz'/.test(appJs) &&
      /viewLayout\.main === 'ptz_vis'/.test(appJs));
check('App 壳只拉当前大屏那一路视频',
      /function wantedTiles/.test(mediaJs) &&
      /inAppShell\(\)/.test(mediaJs) &&
      /main === 'cloud'/.test(mediaJs));
check('网页打开时指标默认隐藏',
      /telemetry[\s\S]*classList\.add\('hidden'\)/.test(appJs) &&
      /class="[^"]*\btelemetry\b[^"]*\bhidden\b/.test(html));
check('网页打开时气体默认隐藏',
      /gas-panel[\s\S]*classList\.add\('hidden'\)/.test(appJs) &&
      (/id="gas-panel"[^>]*\bhidden\b/.test(html) ||
       /class="[^"]*\bgas-panel\b[^"]*\bhidden\b/.test(html)));
check('2×2 时点云保持订阅',
      /cloudVisible/.test(appJs) && /mode === '2x2'/.test(appJs));
var gasCells = (html.match(/id="g-[a-z0-9]+"/g) || []).length;
check('气体面板有 10 个指标', gasCells === 10, '实际 ' + gasCells);

// ---------------------------------------------------------------------------
console.log('\n== 设置面板的字段要与网关认的键一致 ==');
// ---------------------------------------------------------------------------
// 面板多一个键，网关会回 unknown_key 直接拒掉整次保存；少一个键，那项永远改不了。
// 两边都是手写的清单，不比一次就会慢慢长歪。

var hpp = fs.readFileSync(
  path.join(__dirname, '..', 'rk3588', 'include', 'x30', 'gateway_config.hpp'), 'utf8');
var structBody = (hpp.match(/struct GatewaySettings \{([\s\S]*?)\n\};/) || [])[1] || '';
var cppKeys = {};
(structBody.match(/^\s+(?:std::string|uint16_t|uint32_t|bool|int)\s+(\w+)/gm) || [])
  .forEach(function (line) {
    cppKeys[line.trim().split(/\s+/).pop()] = true;
  });

var uiKeys = {};
var kr = /key:\s*'(\w+)'/g, km;
while ((km = kr.exec(read('settings.js'))) !== null) uiKeys[km[1]] = true;

check('读出了网关的配置字段', Object.keys(cppKeys).length > 5,
      Object.keys(cppKeys).length + ' 个');

var extra = Object.keys(uiKeys).filter(function (k) { return !cppKeys[k]; });
var absent = Object.keys(cppKeys).filter(function (k) { return !uiKeys[k]; });
check('面板没有网关不认的字段', extra.length === 0, extra.join(', '));
check('网关的字段面板都能改', absent.length === 0, absent.join(', '));

// ---------------------------------------------------------------------------
console.log('\n== 设置解锁用密码，不再去板子取令牌 ==');
// ---------------------------------------------------------------------------

check('设置面板标签是密码', /set-label">密码</.test(html));
check('设置面板不再提 checkup.sh --token',
      html.indexOf('checkup.sh --token') === -1 &&
      read('settings.js').indexOf('checkup.sh --token') === -1);

console.log('\n通过 ' + pass + '，失败 ' + fail);
process.exit(fail === 0 ? 0 : 1);
