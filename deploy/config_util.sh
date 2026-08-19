# shellcheck shell=bash
# 网关配置文件（gateway.conf）的读写。install.sh、checkup.sh 和几个测试共用。
#
# 只有这一份。安装脚本写、体检脚本读、网关自己也读也写 —— 四个地方各抄一套
# 解析的话，迟早出现「装的时候写进去了、体检读不出来」，而体检会安静地报着
# 默认地址。这正是 render_unit.sh 当初要消灭的那类毛病。
#
# 格式与 rk3588/src/gateway_config.cpp 严格一致：
#   key = value，# 起头是注释，同名键后出现的覆盖先出现的。
# 网关加载时会校验每一项，所以这里写错不会被静默接受 —— 服务会起不来并说明原因。

# 取一个键的值。文件不存在或键不存在时输出空串。
conf_get() {
  local file=$1 key=$2
  [[ -f $file ]] || return 0
  awk -v k="$key" '
    {
      line = $0
      sub(/^[[:space:]]*/, "", line)
      if (line ~ /^#/ || line == "") next
      eq = index(line, "=")
      if (eq == 0) next
      name = substr(line, 1, eq - 1)
      val  = substr(line, eq + 1)
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", name)
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", val)
      if (name == k) { out = val; seen = 1 }
    }
    END { if (seen) print out }
  ' "$file"
}

# 把内置默认值设进下面这组变量。与 GatewaySettings 的默认值保持一致。
conf_defaults() {
  ROBOT_IP="192.168.1.103"
  ROBOT_PORT="43893"
  LOCAL_PORT="43897"
  PERCEPTION_IP="192.168.1.105"
  PERCEPTION_PORT="43899"
  HTTP_PORT="8080"
  BIND_ADDR="0.0.0.0"
  # 点云默认关闭：感知主机的 ROS 可达性没有现场验证过之前，
  # 不该在开机时就去连一台生产设备。
  CLOUD="no"
  ROS_MASTER=""            # 空 = 由感知主机地址推出
  ROS_HOST="192.168.1.120"
  CLOUD_TOPIC="/lidar_points"
  CLOUD_HZ="2"
  CLOUD_POINTS="20000"
}

# 从已有配置文件覆盖这组变量。文件里没有的键保持原值，
# 这样「升级时不带参数重跑 install.sh」不会把控制台改过的配置冲掉。
conf_load() {
  local file=$1 v
  [[ -f $file ]] || return 0
  v=$(conf_get "$file" robot_ip);        [[ -n $v ]] && ROBOT_IP=$v
  v=$(conf_get "$file" robot_port);      [[ -n $v ]] && ROBOT_PORT=$v
  v=$(conf_get "$file" local_port);      [[ -n $v ]] && LOCAL_PORT=$v
  v=$(conf_get "$file" perception_ip);   [[ -n $v ]] && PERCEPTION_IP=$v
  v=$(conf_get "$file" perception_port); [[ -n $v ]] && PERCEPTION_PORT=$v
  v=$(conf_get "$file" http_port);       [[ -n $v ]] && HTTP_PORT=$v
  v=$(conf_get "$file" bind_address);    [[ -n $v ]] && BIND_ADDR=$v
  v=$(conf_get "$file" cloud_enabled);   [[ -n $v ]] && CLOUD=$v
  v=$(conf_get "$file" ros_master);      [[ -n $v ]] && ROS_MASTER=$v
  v=$(conf_get "$file" ros_host);        [[ -n $v ]] && ROS_HOST=$v
  v=$(conf_get "$file" cloud_topic);     [[ -n $v ]] && CLOUD_TOPIC=$v
  v=$(conf_get "$file" cloud_hz);        [[ -n $v ]] && CLOUD_HZ=$v
  v=$(conf_get "$file" cloud_points);    [[ -n $v ]] && CLOUD_POINTS=$v
  return 0
}

# 从旧版布局的 systemd 单元里读参数，同样覆盖那组变量。
#
# 升级前的那一版把地址直接写在 ExecStart 上。装过那一版的板子，升级时若不把这些
# 值搬过来，不带参数重跑 install.sh 就会**悄悄退回默认地址** —— 人明明装的时候
# 指定过 --robot-ip 192.168.1.200，升级完却连到 .103 去了，而且没有任何提示。
#
# 需要先 source deploy/render_unit.sh，这里用它的 unit_exec_args。
# 返回 0 表示确实是旧布局并读到了参数，1 表示不是（新布局或读不出来）。
conf_load_from_unit() {
  local unit=$1 exec_args v
  [[ -f $unit ]] || return 1
  exec_args=$(unit_exec_args "$unit")
  [[ -n ${exec_args// /} ]] || return 1
  # 新布局只有 --config，没有内联的地址参数。
  case " $exec_args " in *" --robot-ip "*) ;; *) return 1 ;; esac

  # -x 是必须的：不加的话 --port 会匹配到 --perception-port 和 --cloud-points。
  unit_arg() { echo "$exec_args" | tr ' ' '\n' | grep -A1 -x -- "$1" | sed -n 2p; }

  v=$(unit_arg --robot-ip);        [[ -n $v ]] && ROBOT_IP=$v
  v=$(unit_arg --robot-port);      [[ -n $v ]] && ROBOT_PORT=$v
  v=$(unit_arg --local-port);      [[ -n $v ]] && LOCAL_PORT=$v
  v=$(unit_arg --perception-ip);   [[ -n $v ]] && PERCEPTION_IP=$v
  v=$(unit_arg --perception-port); [[ -n $v ]] && PERCEPTION_PORT=$v
  v=$(unit_arg --port);            [[ -n $v ]] && HTTP_PORT=$v
  v=$(unit_arg --bind);            [[ -n $v ]] && BIND_ADDR=$v
  v=$(unit_arg --ros-master);      [[ -n $v ]] && ROS_MASTER=$v
  v=$(unit_arg --ros-host);        [[ -n $v ]] && ROS_HOST=$v
  v=$(unit_arg --cloud-topic);     [[ -n $v ]] && CLOUD_TOPIC=$v
  v=$(unit_arg --cloud-hz);        [[ -n $v ]] && CLOUD_HZ=$v
  v=$(unit_arg --cloud-points);    [[ -n $v ]] && CLOUD_POINTS=$v
  case " $exec_args " in *" --cloud "*) CLOUD=yes ;; esac
  return 0
}

# 写出配置文件。读的是上面那组变量。
#
# 用变量而不是十三个位置参数：位置参数多到这个数量时，插错一个位置的后果是
# 端口和地址对调，而两者都"看起来像配置"，没人会一眼看出来。
write_gateway_conf() {
  local file=$1
  local master=$ROS_MASTER
  # ROS master 默认就是感知主机的 11311。不在这里补全的话，改了感知主机
  # 却忘了改 master，点云会去连一台已经不相干的机器。
  [[ -z $master ]] && master="http://$PERCEPTION_IP:11311"

  local dir
  dir=$(dirname "$file")
  mkdir -p "$dir"

  # 先写临时文件再 rename，和网关那边一样：装一半断电不该留下半份配置。
  cat > "$file.tmp" <<EOF
# X30 网关配置。控制台「设置」面板会重写本文件，
# 手写的注释不会保留。改完需要重启服务才生效：
#   systemctl restart x30-gateway

robot_ip = $ROBOT_IP
robot_port = $ROBOT_PORT
local_port = $LOCAL_PORT

perception_ip = $PERCEPTION_IP
perception_port = $PERCEPTION_PORT

http_port = $HTTP_PORT
bind_address = $BIND_ADDR

cloud_enabled = $CLOUD
ros_master = $master
ros_host = $ROS_HOST
cloud_topic = $CLOUD_TOPIC
cloud_hz = $CLOUD_HZ
cloud_points = $CLOUD_POINTS
EOF
  mv -f "$file.tmp" "$file"
}

# 生成管理令牌。改配置能把网关指到别的主机、也能把监听面从内网扩到全部网卡，
# 而协议本身没有身份认证，所以这道门是必须的。
#
# 不用 xxd：它在 Debian/Ubuntu 里属于 vim 包，最小化镜像上常常没有。
# od 属于 coreutils，一定在。
gen_admin_token() {
  if command -v openssl >/dev/null 2>&1; then
    openssl rand -hex 16
  else
    od -An -tx1 -N16 /dev/urandom | tr -d ' \n'
  fi
}
