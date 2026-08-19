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
check('点标题打开设置', /id="btn-settings"[^>]*>\s*X30 遥控台/.test(html));
check('没有单独的订阅点云按钮', html.indexOf('订阅点云') === -1 && !htmlIds['btn-cloud']);
check('点云画面有设置按钮', !!htmlIds['btn-cloud-settings']);
check('点云菜单初始收着',
      /id="cloud-ctl"[^>]*\bhidden\b/.test(html) ||
      /class="[^"]*\bhidden\b[^"]*"[^>]*id="cloud-ctl"/.test(html));

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
console.log('\n== 提示里的取令牌命令必须真的能用 ==');
// ---------------------------------------------------------------------------
// 令牌文件是 600 root，目录 750。提示里漏了 sudo 的话，普通用户照着敲会失败 ——
// 说明书本身跑不通比少个功能更容易把人卡住。

[['index.html', html], ['settings.js', read('settings.js')]].forEach(function (pair) {
  var hits = pair[1].match(/[\w ]*deploy\/checkup\.sh --token/g) || [];
  var bad = hits.filter(function (h) { return !/sudo/.test(h); });
  check(pair[0] + ' 里的取令牌命令带 sudo', bad.length === 0,
        bad.length ? '漏了 sudo：' + bad.join(' / ') : '（' + hits.length + ' 处）');
});

console.log('\n通过 ' + pass + '，失败 ' + fail);
process.exit(fail === 0 ? 0 : 1);
