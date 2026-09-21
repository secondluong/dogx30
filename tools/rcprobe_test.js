// 遥控器通道探测：node tools/rcprobe_test.js
//
// 换 G30 之后通道会整表重排。这一层如果把 CH 号算错（0 基 / 1 基搞反），
// 现场记下来的报告就会差一路，后面整表映射全错。

'use strict';

var P = require('../web/rcprobe.js');

var pass = 0, fail = 0;

function check(name, ok, detail) {
  if (ok) { pass++; console.log('  [OK]   ' + name); }
  else { fail++; console.log('  [FAIL] ' + name + (detail ? '  ' + detail : '')); }
}

console.log('\n== 静止不记事件 ==');
var p = P.makeProbe();
var out = p.feedRc({
  connected: true, device: 'G30', seq: 1,
  ch: [1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500],
});
check('首帧记下松开值', out.rows.length === 9 && out.rows[0].rest === 1500);
check('首帧没有按下记录', out.events.length === 0, JSON.stringify(out.events));

console.log('\n== 按下记 CH 号（从 1 起）==');
out = p.feedRc({
  connected: true, device: 'G30', seq: 2,
  ch: [1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1950],
});
check('CH9 记一条', out.events.length === 1 && out.events[0].ch === 9,
      JSON.stringify(out.events));
check('按下值是 1950', out.events[0] && out.events[0].now === 1950);
check('标成按键（高）', out.events[0] && out.events[0].kind.indexOf('高') >= 0,
      out.events[0] && out.events[0].kind);
check('格子亮着', out.rows[8].moved === true);

out = p.feedRc({
  connected: true, device: 'G30', seq: 3,
  ch: [1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1950],
});
check('按住不重复记', out.events.length === 0, JSON.stringify(out.events));

out = p.feedRc({
  connected: true, device: 'G30', seq: 4,
  ch: [1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500],
});
out = p.feedRc({
  connected: true, device: 'G30', seq: 5,
  ch: [1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1950],
});
check('松开再按再记一条', out.events.length === 1 && out.events[0].ch === 9);

console.log('\n== 低电平按键 ==');
p = P.makeProbe();
p.feedRc({ connected: true, ch: [1500, 1500, 1500] });
out = p.feedRc({ connected: true, ch: [1500, 1050, 1500] });
check('CH2 低按记录', out.events[0] && out.events[0].ch === 2);
check('标成按键（低）', out.events[0] && out.events[0].kind.indexOf('低') >= 0,
      out.events[0] && out.events[0].kind);

console.log('\n== Android 键 ==');
p = P.makeProbe();
check('重复按下不记', p.feedKey({ down: true, repeat: 1, keyCode: 8, name: 'X' }) === null);
var line = p.feedKey({
  down: true, repeat: 0, keyCode: 131, scanCode: 704, name: 'KEYCODE_BUTTON_1',
});
check('记 keyCode 和名字',
      !!line && line.indexOf('131') >= 0 && line.indexOf('KEYCODE_BUTTON_1') >= 0, line);

console.log('\n== 报告 ==');
p = P.makeProbe();
p.feedRc({ connected: true, device: 'G30', ch: [1500, 1500] });
p.feedRc({ connected: true, device: 'G30', ch: [1500, 1950] });
var report = p.report();
check('报告含设备名', report.indexOf('G30') >= 0);
check('报告用 CH1 而不是下标 0',
      report.indexOf('CH1') >= 0 && report.indexOf('CH2') >= 0, report);
check('对照表 CH 从 1 起', P.G20_HINT[0].ch === 1 && P.G20_HINT[8].ch === 9);

console.log('\n通过 ' + pass + '，失败 ' + fail);
process.exit(fail === 0 ? 0 : 1);
