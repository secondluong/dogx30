# shellcheck shell=bash
# 生成 x30-gateway 的 systemd 单元。被 install.sh 和几个测试脚本共用。
#
# 单独拎出来是因为这段 sed 曾经在三个地方各抄了一份：install.sh 真正在用的，
# 加上两个测试里"照着写"的副本。副本的问题不是麻烦，是**它会骗人** ——
# 测试验的是副本，改了 install.sh 忘了改副本，测试照样全绿。
#
# 现在只有这一份。测试拿到的就是装机时真正会用的东西。
#
# 用法：
#   source deploy/render_unit.sh
#   render_unit <模板> <robot> <perception> <local_port> \
#               <http_port> <bind> <cloud:yes|no> <ros_host> <prefix>

render_unit() {
  local template=$1 robot=$2 perception=$3 local_port=$4
  local http_port=$5 bind=$6 cloud=$7 ros_host=$8 prefix=$9

  local cloud_flag=""
  [[ "$cloud" == "yes" ]] && cloud_flag="--cloud "

  # 先剥掉 CR。模板在 Windows 上编辑过就会带 CR，照原样 sed 出去的话，
  # systemd 单元里每个参数尾巴上都挂一个 \r：反斜杠续行会断，
  # 或者 --ros-host 拿到的是 "192.168.1.120\r"。装到板子上才发现就太晚了。
  tr -d '\r' < "$template" |
  sed -e "s|--robot-ip 192.168.1.103|--robot-ip $robot|" \
      -e "s|--perception-ip 192.168.1.105|--perception-ip $perception|" \
      -e "s|--local-port 43897|--local-port $local_port|" \
      -e "s|--port 8080|--port $http_port|" \
      -e "s|--bind 0.0.0.0|--bind $bind|" \
      -e "s|--ros-master http://192.168.1.105:11311|--ros-master http://$perception:11311|" \
      -e "s|--ros-host 192.168.1.120|${cloud_flag}--ros-host $ros_host|" \
      -e "s|/opt/x30|$prefix|g"
}

# 从单元文件里把 ExecStart 的参数抠成一行。
#
# ExecStart 是反斜杠续行的多行，只看第一行的话一个参数都拿不到 ——
# 而调用方通常会"没拿到就用默认值"，于是错得悄无声息。checkup.sh 里
# 有过这个 bug，所以这里也统一成一份。
#
# 先删 CR 再判续行。模板在 Windows 上编辑过就会带 CR，那样行尾是 "\<CR>"，
# 续行判断 /\\$/ 匹配不上，awk 读完第一行就退出 —— 又是"安静地拿不到参数"。
# 这个组合真发生过。
unit_exec_args() {
  tr -d '\r' < "$1" |
    awk '/^ExecStart=/{f=1} f{print} f && !/\\$/{exit}' |
    sed -e 's/^ExecStart=//' -e 's/\\$//' | tr '\n' ' '
}
