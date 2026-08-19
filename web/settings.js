// 网关设置面板：在线改运动主机、感知主机、监听地址与点云参数。
//
// 这些参数以前只存在于 systemd 单元的 ExecStart 里，装完就只能上命令行改。
// 现场换一台感知主机、或者发现板子被路由器 NAT 到别的网段要改绑定地址时，
// 蹲在狗旁边用 vi 编辑 systemd 单元是不现实的。
//
// 三件事必须交代清楚，它们决定了这个面板为什么长这样：
//
// 一、要令牌。协议本身没有身份认证（见 README 安全设计），而改配置能把网关
//     指到别的主机上，也能把监听面从内网扩到全部网卡 —— 比操控狗更值得设门。
//     令牌在板子上，用 sudo deploy/checkup.sh --token 看（文件是 600 root）。
// 二、要重启。所有参数都在启动时装配（UDP 套接字、监听地址、ROS 节点），
//     没有一条能热改。保存后网关会自己重启，遥控中断一两秒。
// 三、要没人在操控。重启会中断遥控，狗正走着的时候不能发生，所以有人持有
//     控制权时网关会直接拒绝。
//
// 不写 ?. 和 ??：安卓 App 是 WebView 壳，旧 WebView 不认这些语法。

(function () {
  'use strict';

  const TOKEN_KEY = 'x30.admin.token';

  const SECTIONS = [
    {
      title: '机器狗连接',
      note: '运动与感知是两台不同的主机，上下楼步态需要两条通道配合。',
      fields: [
        { key: 'robot_ip', label: '运动主机 IP', type: 'text',
          hint: '所有行走控制走这台。' },
        { key: 'robot_port', label: '运动主机端口', type: 'number',
          hint: '官方默认 43893，一般不用改。' },
        { key: 'local_port', label: '本机遥测接收端口', type: 'number',
          hint: '必须与运动主机 network.toml 里为本机登记的端口一致，' +
                '否则一条遥测都收不到。' },
        { key: 'perception_ip', label: '感知主机 IP', type: 'text',
          hint: '上下楼地形图、点云、机身相机都在这台上。' },
        { key: 'perception_port', label: '地形图端口', type: 'number',
          hint: '官方默认 43899。' },
      ],
    },
    {
      title: '遥控服务',
      note: '改这两项会换掉控制台自己的地址，保存后需要用新地址重新打开。',
      fields: [
        { key: 'http_port', label: '服务端口', type: 'number',
          hint: '控制台与 WebSocket 都在这个端口上。' },
        { key: 'bind_address', label: '监听地址', type: 'text',
          hint: '0.0.0.0 表示全部网卡。协议没有身份认证，本机接了 4G ' +
                '就务必填遥控链路那块网卡的地址。' },
      ],
    },
    {
      title: '激光点云',
      note: '感知主机的 ROS 可达性要先验证过再开。',
      fields: [
        { key: 'cloud_enabled', label: '启用点云回传', type: 'bool' },
        { key: 'ros_master', label: 'ROS master', type: 'text',
          hint: '通常是 http://<感知主机>:11311。' },
        { key: 'ros_host', label: '本机在 ROS 网络中的地址', type: 'text',
          hint: '必须是与狗直连那块网卡的地址。填成 MESH 侧地址的话，' +
                '订阅会成功但一帧数据都收不到。' },
        { key: 'cloud_topic', label: '点云话题', type: 'text' },
        { key: 'cloud_hz', label: '下行帧率', type: 'number',
          hint: '话题本身 10 Hz，下行降到 2 Hz 约 1.4 Mbps。' },
        { key: 'cloud_points', label: '单帧点数上限', type: 'number',
          hint: '20000 点约 120 KB/帧。' },
      ],
    },
  ];

  const state = {
    sendFn: null,
    available: false,   // 网关是否以 --config 启动
    token: '',
    current: null,      // 网关回来的那一份，用于算改动
    open: false,
    saving: false,
    waitingRestart: false,
  };

  const inputs = {};
  let root = null;
  let noteEl = null;

  const $ = (id) => document.getElementById(id);

  // -------------------------------------------------------------------------
  // 构建
  // -------------------------------------------------------------------------

  function buildForm(host) {
    SECTIONS.forEach((sec) => {
      const h = document.createElement('div');
      h.className = 'set-sect';
      h.textContent = sec.title;
      host.appendChild(h);

      if (sec.note) {
        const n = document.createElement('p');
        n.className = 'set-sect-note';
        n.textContent = sec.note;
        host.appendChild(n);
      }

      sec.fields.forEach((f) => {
        const row = document.createElement('label');
        row.className = 'set-row';

        const name = document.createElement('span');
        name.className = 'set-label';
        name.textContent = f.label;
        row.appendChild(name);

        const input = document.createElement('input');
        if (f.type === 'bool') {
          input.type = 'checkbox';
          input.className = 'set-check';
        } else {
          input.type = f.type === 'number' ? 'number' : 'text';
          input.className = 'set-input';
          input.autocomplete = 'off';
          input.spellcheck = false;
        }
        inputs[f.key] = input;
        row.appendChild(input);

        if (f.hint) {
          const hint = document.createElement('small');
          hint.className = 'set-hint';
          hint.textContent = f.hint;
          row.appendChild(hint);
        }

        host.appendChild(row);
      });
    });

    // ROS master 默认就是「感知主机的 11311」。改了感知主机却忘了改这里，
    // 表现是点云连不上一台已经不存在的 master。跟着改，并且是看得见地改 ——
    // 悄悄改和不改一样糟。
    inputs.perception_ip.addEventListener('change', () => {
      if (!state.current) return;
      const old = 'http://' + state.current.perception_ip + ':11311';
      if (inputs.ros_master.value.trim() !== old) return;
      const next = 'http://' + inputs.perception_ip.value.trim() + ':11311';
      inputs.ros_master.value = next;
      inputs.ros_master.classList.add('set-auto');
      setNote('ROS master 已跟着感知主机改成 ' + next + '，不对的话直接改它。');
    });
  }

  // -------------------------------------------------------------------------
  // 读写表单
  // -------------------------------------------------------------------------

  function fillForm(s) {
    Object.keys(inputs).forEach((key) => {
      const input = inputs[key];
      if (input.type === 'checkbox') {
        input.checked = !!s[key];
      } else {
        input.value = s[key];
      }
      input.classList.remove('set-auto');
    });
    updateCloudRows();
  }

  // 只收改动过的字段。整份回传的话，将来网关多出一个本面板不认识的参数时，
  // 会被这边的旧值覆盖掉。
  function changedFields() {
    const out = {};
    if (!state.current) return out;
    Object.keys(inputs).forEach((key) => {
      const input = inputs[key];
      if (input.type === 'checkbox') {
        if (input.checked !== !!state.current[key]) out[key] = input.checked;
        return;
      }
      const raw = input.value.trim();
      if (input.type === 'number') {
        const n = Number(raw);
        if (raw !== '' && !isNaN(n) && n !== state.current[key]) out[key] = n;
      } else if (raw !== '' && raw !== state.current[key]) {
        out[key] = raw;
      }
    });
    return out;
  }

  // 点云关着的时候那几项没有意义，灰掉但不隐藏 —— 隐藏会让人以为不能配。
  function updateCloudRows() {
    const on = inputs.cloud_enabled.checked;
    ['ros_master', 'ros_host', 'cloud_topic', 'cloud_hz', 'cloud_points']
      .forEach((key) => {
        inputs[key].disabled = !on;
        inputs[key].parentNode.classList.toggle('set-off', !on);
      });
  }

  // -------------------------------------------------------------------------
  // 交互
  // -------------------------------------------------------------------------

  function setNote(text, kind) {
    noteEl.textContent = text || '';
    noteEl.className = 'set-note' + (kind ? ' ' + kind : '');
    noteEl.classList.toggle('hidden', !text);
  }

  function setLocked(locked) {
    root.classList.toggle('set-locked', locked);
    $('set-unlock-box').classList.toggle('hidden', !locked);
    $('set-form').classList.toggle('hidden', locked);
    $('set-save').classList.toggle('hidden', locked);
  }

  function open() {
    if (!state.available) return;
    state.open = true;
    root.classList.remove('hidden');
    if (state.current) {
      setLocked(false);
    } else {
      setLocked(true);
      const saved = readSavedToken();
      if (saved) {
        $('set-token').value = saved;
        unlock();
      }
    }
  }

  function close() {
    state.open = false;
    root.classList.add('hidden');
    setNote('');
  }

  function readSavedToken() {
    try { return window.localStorage.getItem(TOKEN_KEY) || ''; } catch (e) { return ''; }
  }

  function saveToken(token) {
    try {
      if ($('set-remember').checked) {
        window.localStorage.setItem(TOKEN_KEY, token);
      } else {
        window.localStorage.removeItem(TOKEN_KEY);
      }
    } catch (e) { /* 隐私模式下写不了，不影响本次使用 */ }
  }

  function unlock() {
    const token = $('set-token').value.trim();
    if (!token) {
      setNote('请填管理令牌。在板子上执行 sudo bash deploy/checkup.sh --token 可以看到它。', 'bad');
      return;
    }
    state.token = token;
    setNote('正在读取当前配置…');
    state.sendFn({ t: 'config_get', token: token });
  }

  function save() {
    const diff = changedFields();
    if (Object.keys(diff).length === 0) {
      setNote('没有任何改动。', 'warn');
      return;
    }

    const portChange = Object.prototype.hasOwnProperty.call(diff, 'http_port');
    const bindChange = Object.prototype.hasOwnProperty.call(diff, 'bind_address');
    let confirmText = '保存后网关会重启，遥控会中断一两秒。继续？';
    if (portChange || bindChange) {
      confirmText = '改了服务端口或监听地址，重启后这个页面的地址可能就不通了，' +
                    '需要用新地址重新打开。继续？';
    }
    if (!window.confirm(confirmText)) return;

    state.saving = true;
    $('set-save').disabled = true;
    setNote('正在保存…');
    state.sendFn({ t: 'config_set', token: state.token, settings: diff });
  }

  // -------------------------------------------------------------------------
  // 网关回执
  // -------------------------------------------------------------------------

  function onConfig(msg) {
    state.current = msg.settings;
    saveToken(state.token);
    fillForm(msg.settings);
    setLocked(false);

    if (state.waitingRestart) {
      state.waitingRestart = false;
      setNote('网关已带着新配置起来了。', 'ok');
      return;
    }
    if (msg.auto_restart) {
      setNote('改完保存，网关会自己重启并在一两秒后恢复。');
    } else {
      setNote('本机不是由 systemd 托管的，保存后需要自己重启网关才生效。', 'warn');
    }
  }

  function onConfigSaved(msg) {
    state.saving = false;
    $('set-save').disabled = false;
    state.current = msg.settings;
    fillForm(msg.settings);

    // 端口变了的话这个页面连不回来，必须把新地址明确告诉人，
    // 否则只会看到一个永远在「重连中」的界面。
    const newPort = String(msg.settings.http_port);
    const nowPort = location.port || (location.protocol === 'https:' ? '443' : '80');
    if (newPort !== nowPort) {
      state.waitingRestart = false;
      setNote('已保存。服务端口改成了 ' + newPort + '，这个页面不会自己回来，' +
              '请打开 ' + location.protocol + '//' + location.hostname + ':' +
              newPort + '/', 'warn');
      return;
    }

    if (msg.auto_restart) {
      state.waitingRestart = true;
      setNote('已保存，网关正在重启，等它回来…');
    } else {
      setNote('已保存。本机不由 systemd 托管，请手动重启：' +
              'systemctl restart x30-gateway', 'warn');
    }
  }

  // 配置相关的报错要显示在面板里。丢给顶部横幅的话，人正盯着表单，
  // 四秒后横幅消失，只留下一个「点了保存好像没反应」。
  function onError(msg) {
    const mine = msg.code === 'bad_admin_token' || msg.code === 'no_admin_token' ||
                 msg.code === 'bad_config' || msg.code === 'busy_control' ||
                 msg.code === 'config_write_failed' || msg.code === 'no_config';
    if (!mine || !state.open) return false;

    state.saving = false;
    $('set-save').disabled = false;
    setNote(msg.msg, 'bad');
    if (msg.code === 'bad_admin_token' || msg.code === 'no_admin_token') {
      state.current = null;
      setLocked(true);
    }
    return true;
  }

  // 断线时把「已解锁」的状态留着（令牌还在内存里），但要让人知道现在没连上。
  function onLink(online) {
    if (online || !state.open) return;
    if (state.waitingRestart) {
      setNote('网关正在重启，等它回来…');
    } else if (state.saving) {
      setNote('连接断了，这次保存不确定有没有成功，重连后请核对一遍。', 'warn');
      state.saving = false;
      $('set-save').disabled = false;
    }
  }

  // 重连上来后主动再取一次，确认新配置真的生效了 —— 只是「存下来了」
  // 和「跑起来了」是两件事。
  function onHello(msg) {
    state.available = !!msg.config;
    const btn = $('btn-settings');
    if (btn) btn.classList.toggle('hidden', !state.available);
    if (!state.available) {
      if (state.open) {
        setNote('本机未启用在线改配置（网关需要以 --config 启动）。', 'warn');
      }
      return;
    }
    if (state.open && state.token) {
      state.sendFn({ t: 'config_get', token: state.token });
    }
  }

  function initSettings(send) {
    state.sendFn = send;
    root = $('settings');
    noteEl = $('set-note');
    if (!root) return;

    buildForm($('set-form'));

    $('btn-settings').addEventListener('click', open);
    $('set-close').addEventListener('click', close);
    $('set-unlock').addEventListener('click', unlock);
    $('set-save').addEventListener('click', save);
    inputs.cloud_enabled.addEventListener('change', updateCloudRows);

    $('set-token').addEventListener('keydown', (e) => {
      if (e.key === 'Enter') unlock();
    });

    // 点遮罩关闭，但不要在点面板内部时也关掉。
    root.addEventListener('click', (e) => {
      if (e.target === root) close();
    });
  }

  window.X30Settings = {
    initSettings, onConfig, onConfigSaved, onError, onHello, onLink,
  };
})();
