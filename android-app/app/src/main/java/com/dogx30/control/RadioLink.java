package com.dogx30.control;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;

import androidx.annotation.Nullable;

import com.skydroid.rcsdk.KeyManager;
import com.skydroid.rcsdk.PipelineManager;
import com.skydroid.rcsdk.comm.CommListener;
import com.skydroid.rcsdk.common.callback.CompletionCallback;
import com.skydroid.rcsdk.common.error.SkyException;
import com.skydroid.rcsdk.common.pipeline.Pipeline;
import com.skydroid.rcsdk.key.AirLinkKey;

import org.json.JSONObject;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.InterfaceAddress;
import java.net.Socket;
import java.io.FileDescriptor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.net.NetworkInterface;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Enumeration;

/**
 * 厂家拓扑：手柄 --2.4G--> 接收机 --交换机--> 运动主机 192.168.1.103:43893。
 * G20 射频起来后是虚口 ar_net0，系统 ConnectivityManager 看不见。
 * Java UDP 必须 SO_BINDTODEVICE，否则全是 Network unreachable。
 */
final class RadioLink {

    private static final String TAG = "RadioLink";
    private static final String ROBOT_IP = "192.168.1.103";
    private static final int ROBOT_PORT = 43893;
    private static final int LOCAL_PORT = 43897;
    private static final int TICK_MS = 20;
    private static final int HB_EVERY = 10;
    /** 卸力到起立之间的间隔。太短运动主机还在切状态，会把起立丢掉。 */
    private static final int STAND_AFTER_UNLOAD_MS = 400;
    private static final int AXIS_MAX = 32767;
    private static final int AXIS_DZ = 655;

    // 运动主机的遥测。0x1009 那一包头后第一个字节就是 basic_state，第二个是 gait_state
    // （见 rk3588/include/x30/protocol.hpp 的 CommandHead + MotionStateData）。
    // 这是狗**自己报的**姿态，比本地猜靠得住：以前这里只数包不看内容，
    // 一切档本地那份猜测就和实际脱节，界面便一直显示站立。
    private static final int TELEM_RUNNING = 0x1008;
    private static final int TELEM_MOTION = 0x1009;
    /** 身高档位是单独一条简单报文，档位就放在头里的 paramters_size，按有符号读。 */
    private static final int TELEM_HEIGHT = 0x11050F08;
    /** RcsData.is_nav_mode / emergency_source：头 12 字节之后偏移 72/73。 */
    private static final int RCS_NAV_OFF = 72;
    private static final int RCS_EMERG_OFF = 73;
    private static final int RCS_LEN = 88;
    private static final int HEAD_LEN = 12;
    /** 遥测 200 Hz，半秒没有就当它不可信，退回本地记的那份。 */
    private static final long TELEM_FRESH_MS = 500;

    // basic_state 取值，与 protocol.hpp 的 BasicState 一致。
    private static final int ST_SITTING = 0;
    private static final int ST_SIT_TO_STAND = 1;
    private static final int ST_INITIAL_STANDING = 2;
    private static final int ST_TORQUE_STANDING = 3;
    private static final int ST_STEPPING = 4;
    private static final int ST_STAND_TO_SIT = 5;
    private static final int ST_EMERGENCY = 6;

    /** 会改变姿态的那几条。发过它们，本机记的「站没站」才有依据。 */
    private static final java.util.Set<String> POSE_CMDS = new java.util.HashSet<>(
            java.util.Arrays.asList("stand", "stand_up", "sit", "sit_down",
                                    "unload", "estop"));

    /** 起立刚发出去的这一小段不发轴，免得把柔和的起身掐硬。 */
    private static final long AXIS_AFTER_STAND_MS = 400;
    /** 屏幕摇杆多久没更新就当松手。掉帧或页面卡住时不能让狗一直走。 */
    private static final long SCREEN_AXIS_HOLD_MS = 400;

    private static final int HEARTBEAT = 0x21040001;
    private static final int CONNECT = 0x21020001;
    private static final int STAND = 0x21010223;
    private static final int SIT = 0x21010222;
    private static final int UNLOAD = 0x21010202;
    private static final int TORQUE = 0x2101020A;
    private static final int STEP = 0x21010201;
    private static final int ESTOP = 0x21010C0E;
    private static final int MODE_MANUAL = 0x21010C02;
    private static final int MODE_AUTO = 0x21010C03;
    private static final int HEIGHT = 0x21010406;
    private static final int AXIS_LY = 0x21010130;
    private static final int AXIS_LX = 0x21010131;
    private static final int AXIS_RX = 0x21010135;
    private static final int AXIS_RY = 0x21010102;
    private static final long STEP_AFTER_TORQUE_MS = 500;

    private static final RadioLink INST = new RadioLink();

    static RadioLink get() {
        return INST;
    }

    @Nullable private HandlerThread worker;
    @Nullable private Handler handler;
    private boolean enabled;
    private boolean running;
    private boolean confirmed;
    private boolean standing;
    private int telemState = -1;
    private int telemGait = -1;
    private int telemEmergSrc;
    private int telemNavMode;
    private String lastGaitSent = "";
    private String pendingGait = "";
    private boolean telemRunSeen;
    /** 身高档位：-1 匍匐、0 正常。狗只在变化时报，所以不设新鲜期，收到过就一直算。 */
    private int telemHeight;
    private boolean telemHeightSeen;
    private long telemAt;
    private long lastStandAt;
    private float scrFwd;
    private float scrLat;
    private float scrTurn;
    private float scrTilt;
    private long scrAt;
    /** 这一档里确实发出去过起立/趴下，或者 MESH 那侧交接过来 —— 否则姿态就是瞎猜。 */
    private boolean poseKnown;
    private boolean torqued;
    private boolean stepping;
    private boolean stepPending;
    private boolean stepSent;
    /** effAxes 的返回缓冲。每 tick 都要算一次，没必要每次新建。 */
    private final float[] axBuf = new float[4];
    private boolean emergency;
    private boolean prevStand;
    private boolean prevSit;
    private boolean prevEstop;
    private boolean buttonsPrimed;
    private int tick;
    private int sentOk;
    private int sentFail;
    private int rxOk;
    private String lastCmd = "";
    private boolean pipeOk;
    private boolean rfOn;
    private String status = "off";
    private String lastErr = "";
    private String ifaceName = "";
    private int ifacePrefix;
    @Nullable private Context appCtx;
    @Nullable private DatagramSocket udp;
    @Nullable private InetAddress robotAddr;
    @Nullable private InetAddress boundLocal;
    @Nullable private Network airNet;
    @Nullable private Pipeline udpPipe;
    @Nullable private ConnectivityManager.NetworkCallback netCb;
    @Nullable private ConnectivityManager.NetworkCallback wifiCb;
    /** UDP 那个 socket 真绑上了图传网卡（bindSocket 或 SO_BINDTODEVICE 成功）。 */
    private boolean udpBound;
    /** 当前钉住的进程默认网络，null 表示没钉、跟系统默认走。 */
    @Nullable private Network pinnedNet;

    private final Runnable loop = this::onTick;
    private final Runnable standTask = this::standNow;
    private final Runnable stepTask = this::firePendingStep;

    synchronized void attach(Context ctx) {
        if (ctx != null) appCtx = ctx.getApplicationContext();
        ensureWorker();
        watchWifi();
    }

    // 下面三个是给 NativeVideo 用的：2.4G 下机身相机的 RTSP 也得走这张网卡，
    // 和这里的 UDP 面临同一个「安卓按默认网络路由」的坑，所以把绑定所需的三样
    // 东西一并露出去，而不是让它自己再找一遍网卡。

    /** 系统能看见这条链路时的 Network，看不见则为 null（ar_net0 常见如此）。 */
    @Nullable
    synchronized Network airNetwork() {
        return airNet;
    }

    /** 2.4G 那张网卡上的本机地址，未就绪时为 null。 */
    @Nullable
    synchronized InetAddress localAddress() {
        return boundLocal;
    }

    /** 2.4G 那张网卡的名字（如 ar_net0），未就绪时为空串。 */
    synchronized String ifaceName() {
        return ifaceName == null ? "" : ifaceName;
    }

    private void ensureWorker() {
        if (handler != null) return;
        worker = new HandlerThread("radio-udp");
        worker.start();
        handler = new Handler(worker.getLooper());
    }

    private void onRadio(Runnable r) {
        ensureWorker();
        if (Looper.myLooper() == handler.getLooper()) r.run();
        else handler.post(r);
    }

    void setEnabled(boolean on) {
        onRadio(() -> {
            if (enabled == on) return;
            enabled = on;
            if (on) start();
            else stop();
        });
    }

    /** CSDK 连上遥控器后 USB 网才稳定，再开 socket / 管道。 */
    void onRcReady() {
        onRadio(() -> {
            if (!enabled) return;
            if (udp == null && !pipeOk) status = "rc-up";
            openUdp();
        });
    }

    private boolean telemFresh() {
        return telemAt != 0 && System.currentTimeMillis() - telemAt < TELEM_FRESH_MS;
    }

    /** 关节自锁。急停后主机常回报坐下，原厂看 0x1008 的来源字节。 */
    private boolean telemLocked() {
        if (telemFresh() && (telemState == ST_EMERGENCY
                || (telemEmergSrc >= 4 && telemEmergSrc <= 6))) {
            return true;
        }
        return emergency;
    }

    private boolean telemUpright() {
        // 与 protocol.hpp TelemUpright 一致：初始站立 / 力控站立 / 踏步。
        return telemState == ST_INITIAL_STANDING || telemState == ST_TORQUE_STANDING
                || telemState == ST_STEPPING;
    }

    /**
     * 狗是不是站着。有遥测就信遥测 —— 这是 MESH 那侧用的同一个字段，
     * 两条链路读同一份真相，切档时状态才不会走散。
     */
    synchronized boolean isStanding() {
        if (telemLocked()) return false;
        if (telemFresh()) {
            if (telemUpright()) return true;
            // 起立中 / 坐下中按意图算，否则按钮会在过渡期来回跳。
            if (telemState == ST_SIT_TO_STAND || telemState == ST_STAND_TO_SIT) {
                return standing;
            }
            // RL 起立后运动主机仍报坐下（见 protocol.hpp），这种只能按我们记的算。
            if (telemState == ST_SITTING) return standing;
            return false;
        }
        return standing;
    }

    synchronized boolean isLinkReady() {
        return enabled && sentOk > 0;
    }

    synchronized String statusJson() {
        try {
            JSONObject o = new JSONObject();
            o.put("enabled", enabled);
            o.put("ready", isLinkReady());
            o.put("status", status);
            o.put("udp", udp != null);
            o.put("pipe", pipeOk);
            o.put("rf", rfOn);
            o.put("air", airNet != null);
            // socket 绑没绑上那张网卡。没绑上就只能靠系统默认路由，此时进程也不会
            // 被钉到 WiFi 上（见 pinProcess），MESH 那侧会跟着受影响。
            o.put("bound", udpBound);
            o.put("local", boundLocal != null ? boundLocal.getHostAddress() : airLocalIp());
            o.put("iface", ifaceName);
            o.put("prefix", ifacePrefix);
            o.put("nets", allV4());
            o.put("ifaces", osIfaces());
            o.put("err", lastErr);
            o.put("sentOk", sentOk);
            o.put("sentFail", sentFail);
            o.put("rx", rxOk);
            o.put("cmd", lastCmd);
            o.put("standing", isStanding());
            // 狗自己报的姿态与步态。没有遥测时给 -1，网页那侧就不会拿它盖掉
            // 网关的读数（两条链路共用 app.basicState）。
            o.put("basic", telemFresh() && telemState >= 0 ? telemState : -1);
            o.put("gait", telemFresh() && telemState >= 0 ? telemGait : -1);
            if (telemHeightSeen) o.put("height", telemHeight);
            o.put("emergency", telemLocked());
            o.put("emergSrc", telemEmergSrc);
            o.put("axes", axesApply());
            o.put("poseKnown", poseIsKnown());
            return o.toString();
        } catch (Exception e) {
            return "{}";
        }
    }

    /**
     * 采纳网页告知的姿态。切档时由 MESH 那侧交接过来：网关知道狗站着，而本机刚被
     * 打开、什么都不知道。不发任何指令，只对上记忆 —— 否则从 MESH 切到 2.4G 后
     * 按钮显示「起立」，而且 axesApply 认为狗趴着，推杆没反应。
     */
    void adoptPosture(boolean up) {
        // 必须排在开/关这一档的后面执行：setEnabled 是投到这个线程上的，
        // 而切档就在同一瞬间发生。直接在调用线程上写，快速来回切档时那次
        // stop() 会后到，把刚交接过来的姿态清掉 —— 又变成「切档姿态丢了」。
        onRadio(() -> adoptOnRadio(up));
    }

    private synchronized void adoptOnRadio(boolean up) {
        standing = up;
        poseKnown = true;
        // 姿态是别人告知的，不是我们刚发的起立，所以不必再等身子稳住。
        lastStandAt = 0;
        // 力控/踏步没人告知，先按最保守的算；有遥测时下一帧就纠回来。
        clearWalk();
    }

    /** 姿态到底是知道的还是猜的。猜的就别去改网关那份记忆。 */
    private boolean poseIsKnown() {
        return telemFresh() || poseKnown;
    }

    /**
     * 屏幕摇杆推到哪。通道约定与网关的速度接口一致（前为正、左为正、逆时针为正），
     * 到协议轴的取反在 sendAxes 里做，和实体摇杆走同一条出口。
     */
    synchronized void setScreenAxes(float fwd, float lat, float turn) {
        setScreenAxes(fwd, lat, turn, 0f);
    }

    synchronized void setScreenAxes(float fwd, float lat, float turn, float tilt) {
        scrFwd = fwd;
        scrLat = lat;
        scrTurn = turn;
        scrTilt = tilt;
        scrAt = System.currentTimeMillis();
    }

    /**
     * 网页记的力控/起步。B1/B2 和屏幕按钮都先写 walkMode，本机指令若被
     * 遥测冲掉，这里再对上，否则实体摇杆已经推了轴却发不出去。
     */
    synchronized void adoptWalkMode(String mode) {
        if ("step".equals(mode)) {
            torqued = true;
            stepping = true;
        } else if ("torque".equals(mode)) {
            torqued = true;
            stepping = false;
        }
    }

    private void clearWalk() {
        torqued = false;
        stepping = false;
        cancelPendingStep();
    }

    private void cancelPendingStep() {
        stepPending = false;
        stepSent = false;
        if (handler != null) handler.removeCallbacks(stepTask);
    }

    private synchronized void firePendingStep() {
        if (!enabled || !stepPending) return;
        stepPending = false;
        stepSent = true;
        sendSimple(STEP);
        flushPendingGait();
    }

    private void startStepping() {
        if (emergency) return;
        if (stepping && (stepSent || stepPending)) return;
        sendSimple(TORQUE);
        torqued = true;
        stepping = true;
        standing = true;
        stepSent = false;
        stepPending = true;
        onRadioDelayed(stepTask, STEP_AFTER_TORQUE_MS);
    }

    private void stopStepping() {
        if (emergency) return;
        boolean cancelOnly = stepPending && !stepSent;
        boolean sent = stepSent;
        cancelPendingStep();
        stepping = false;
        torqued = true;
        standing = true;
        if (!cancelOnly && sent) sendSimple(STEP);
    }

    /**
     * 轴能不能发。规则与网关那侧的 protocol.hpp AxisCommandsApply 保持一致。
     *
     * 力控才发姿态轴，起步才发速度轴。起立本身不发，否则没踏步也在喂速度。
     *
     * 起立中 / 坐下中 / 急停一律不发，免得把柔和起身掐硬。
     */
    private boolean axesApply() {
        if (telemLocked()) return false;
        if (lastStandAt != 0
                && System.currentTimeMillis() - lastStandAt < AXIS_AFTER_STAND_MS) {
            return false;
        }
        if (!torqued && !stepping) return false;
        if (telemFresh()) {
            if (telemState == ST_SIT_TO_STAND || telemState == ST_STAND_TO_SIT) {
                return false;
            }
        }
        return true;
    }

    /**
     * 起立总是先卸力再站。
     *
     * 运动主机可能在我们连上之前就被急停锁住（开机抖一下就会），本机的 emergency
     * 标志看不到这种情况，只发 0x21010223 会被静默忽略 —— 现场表现就是「按了没反应」。
     * 狗趴着时卸力没有副作用，所以无条件先发一发。
     */
    private void standUp() {
        sendSimple(UNLOAD);
        emergency = false;
        standing = false;
        clearWalk();
        onRadioDelayed(standTask, STAND_AFTER_UNLOAD_MS);
    }

    /** 卸力后那一发起立还没到，用户又按了趴下/急停，就别再站起来了。 */
    private void cancelPendingStand() {
        if (handler != null) handler.removeCallbacks(standTask);
    }

    private synchronized void standNow() {
        if (!enabled) return;
        sendSimple(STAND);
        standing = true;
        lastStandAt = System.currentTimeMillis();
        clearWalk();
    }

    private void onRadioDelayed(Runnable r, long delayMs) {
        ensureWorker();
        handler.postDelayed(r, delayMs);
    }

    void command(String name) {
        if (name == null) return;
        onRadio(() -> commandOnRadio(name));
    }

    private synchronized void commandOnRadio(String name) {
        lastCmd = name;
        // 发过起立/趴下这一类，本机记的姿态才算有依据；否则切回 MESH 时不该拿它去
        // 改网关的记忆（那边可能记着别人刚把狗扶起来）。
        if (POSE_CMDS.contains(name)) poseKnown = true;
        if (!enabled) {
            enabled = true;
            start();
        }
        if (udp == null) openUdp();
        switch (name) {
            case "stand_up":
                standUp();
                break;
            case "sit":
            case "sit_down":
                cancelPendingStand();
                pendingGait = "";
                if (telemNavMode == 1) sendSimple(MODE_MANUAL);
                sendSimple(SIT);
                standing = false;
                clearWalk();
                break;
            case "stand":
                if (standing && !emergency) {
                    sendSimple(SIT);
                    standing = false;
                    clearWalk();
                } else {
                    standUp();
                }
                break;
            case "unload":
                cancelPendingStand();
                sendSimple(UNLOAD);
                emergency = false;
                standing = false;
                clearWalk();
                break;
            case "torque":
                if (stepping && stepSent) sendSimple(STEP);
                cancelPendingStep();
                sendSimple(TORQUE);
                torqued = true;
                stepping = false;
                break;
            case "step_on":
                startStepping();
                break;
            case "step_off":
                stopStepping();
                break;
            case "step":
                if (stepping || stepPending) stopStepping();
                else startStepping();
                break;
            case "estop":
                cancelPendingStand();
                sendSimple(ESTOP);
                emergency = true;
                standing = false;
                clearWalk();
                break;
            case "manual":
            case "mode":
                sendSimple(MODE_MANUAL);
                break;
            case "auto":
                sendSimple(MODE_AUTO);
                break;
            case "height_low":
                sendSimple(HEIGHT, 0);
                break;
            case "height_normal":
                sendSimple(HEIGHT, 2);
                break;
            default:
                if (name.startsWith("gait_")) {
                    sendGait(name.substring(5));
                } else if (gaitCode(name) != 0) {
                    sendGait(name);
                }
                break;
        }
    }

    private void sendGait(String gait) {
        int code = gaitCode(gait);
        if (code == 0) return;
        boolean leavingStair = isStairGait(lastGaitSent)
                || telemGait == 6 || telemGait == 7 || telemGait == 8;
        if (!isStairGait(gait) && leavingStair) {
            sendSimple(MODE_MANUAL);
            telemNavMode = 0;
        }
        // 没在人按的起步里就只记下。不要等遥测报踏步：RL 起立后常年报 0，
        // 等它等于静音、低姿永远发不出去。
        if (!isStairGait(gait) && !stepping) {
            pendingGait = gait;
            lastGaitSent = gait;
            return;
        }
        sendSimple(code, 0);
        pendingGait = "";
        lastGaitSent = gait;
    }

    private void flushPendingGait() {
        if (pendingGait.isEmpty() || isStairGait(pendingGait)) return;
        int code = gaitCode(pendingGait);
        if (code != 0) sendSimple(code, 0);
        pendingGait = "";
    }

    private static boolean isStairGait(String gait) {
        return "stair".equals(gait) || "stairmulti".equals(gait)
                || "stair45".equals(gait);
    }

    private int gaitCode(String gait) {
        switch (gait) {
            case "walk": return 0x21010300;
            case "slope": return 0x21010402;
            case "offroad": return 0x21010401;
            case "stair": return 0x21010405;
            case "stairmulti": return 0x2101040A;
            case "stair45": return 0x2101040B;
            case "lwalk": return 0x21010420;
            case "mountain": return 0x21010421;
            case "silent": return 0x21010422;
            default: return 0;
        }
    }

    private synchronized void start() {
        confirmed = false;
        buttonsPrimed = false;
        tick = 0;
        sentOk = 0;
        sentFail = 0;
        rxOk = 0;
        lastCmd = "";
        lastErr = "";
        status = "starting";
        enableRf(true);
        watchEthernet();
        openUdp();
        if (!running) {
            running = true;
            handler.post(loop);
        }
    }

    private synchronized void openUdp() {
        openSocket();
        // 绑成没绑成，决定了敢不敢把进程钉到 WiFi 上，所以每次开关 socket 都重算。
        pinProcess();
    }

    private synchronized void openSocket() {
        try {
            AirIf air = findAirIf();
            Network net = findAirlink();
            if (air == null && net == null) {
                if (udp == null) status = "no-usb-net";
                Log.w(TAG, "no-usb-net ifaces=" + osIfaces());
                return;
            }
            InetAddress local = air != null ? air.addr : null;
            String name = air != null ? air.name : "";
            if (udp != null && boundLocal != null && boundLocal.equals(local)
                    && name.equals(ifaceName)
                    && (net == null || net.equals(airNet))) {
                return;
            }
            closeSocketOnly();
            airNet = net;
            boundLocal = local;
            ifaceName = name;
            ifacePrefix = air != null ? air.prefix : 0;
            robotAddr = net != null ? resolve(net, ROBOT_IP) : InetAddress.getByName(ROBOT_IP);
            udp = new DatagramSocket((java.net.SocketAddress) null);
            udp.setReuseAddress(true);
            // 运动主机 network.toml 登记的是 43897；随机端口发出去也不回遥测。
            InetSocketAddress bindAddr = local != null
                    ? new InetSocketAddress(local, LOCAL_PORT)
                    : new InetSocketAddress(LOCAL_PORT);
            udp.bind(bindAddr);
            udp.setSoTimeout(1);
            if (net != null) {
                try {
                    net.bindSocket(udp);
                    udpBound = true;
                } catch (Exception e) {
                    Log.w(TAG, "bindSocket", e);
                }
            }
            // bindSocket 失败也退回按网卡名绑：这个 socket 绑没绑上，决定了后面
            // 敢不敢把进程钉到 WiFi 上，能多一种绑法就多一种。
            if (!udpBound && !name.isEmpty()) {
                try {
                    bindToDevice(udp, name);
                    udpBound = true;
                } catch (Exception e) {
                    lastErr = "bind:" + brief(e);
                    Log.w(TAG, "bindToDevice", e);
                }
            }
            status = "udp-" + ROBOT_IP;
            lastErr = "";
            Log.i(TAG, status + " " + name + "=" + (local != null ? local.getHostAddress() : "-")
                    + "/" + ifacePrefix);
        } catch (Exception e) {
            status = "udp-fail";
            lastErr = brief(e);
            Log.w(TAG, "udp", e);
            closeSocketOnly();
        }
    }

    @Nullable
    private static InetAddress resolve(Network net, String ip) {
        try {
            return net.getByName(ip);
        } catch (Exception e) {
            try {
                return InetAddress.getByName(ip);
            } catch (Exception e2) {
                return null;
            }
        }
    }

    private synchronized void openSdkUdp() {
        if (pipeOk && udpPipe != null) return;
        if (udpPipe != null) {
            closeSdkUdp();
        }
        udpPipe = openPipe(LOCAL_PORT, ROBOT_IP, "udp103");
    }

    @Nullable
    private Pipeline openPipe(int localPort, String ip, String tag) {
        try {
            Pipeline p = PipelineManager.INSTANCE.createUDPPipeline(
                    localPort, ip, ROBOT_PORT);
            if (p == null) return null;
            p.setOnCommListener(pipeListener(tag, true));
            PipelineManager.INSTANCE.connectPipeline(p);
            return p;
        } catch (Throwable t) {
            Log.w(TAG, tag, t);
            return null;
        }
    }

    private CommListener pipeListener(final String tag, final boolean udpKind) {
        return new CommListener() {
            @Override
            public void onConnectSuccess() {
                if (udpKind) {
                    pipeOk = true;
                    status = "pipe-" + ROBOT_IP;
                }
                Log.i(TAG, tag + " up");
            }

            @Override
            public void onConnectFail(@Nullable SkyException e) {
                if (udpKind) pipeOk = false;
                if (e != null) lastErr = "pipe " + e;
                Log.w(TAG, tag + " fail " + e);
            }

            @Override
            public void onDisconnect() {
                if (udpKind) pipeOk = false;
            }

            @Override
            public void onReadData(@Nullable byte[] data) {
            }
        };
    }

    private void closeSdkUdp() {
        pipeOk = false;
        if (udpPipe != null) {
            try {
                PipelineManager.INSTANCE.disconnectPipeline(udpPipe);
            } catch (Throwable ignored) {
            }
            udpPipe = null;
        }
    }

    private void enableRf(boolean on) {
        try {
            KeyManager.INSTANCE.set(
                    AirLinkKey.INSTANCE.getKeyRCRFEnable(),
                    Boolean.valueOf(on),
                    new CompletionCallback() {
                        @Override
                        public void onResult(@Nullable SkyException e) {
                            rfOn = on && e == null;
                            Log.i(TAG, "RF " + on + " " + e);
                        }
                    });
        } catch (Throwable t) {
            Log.w(TAG, "RF", t);
        }
    }

    /**
     * 只是「盯着」图传网卡，好在它上线时重开 socket。
     *
     * 以前这里是主动申请（cm.requestNetwork）：那会把这条网络拉起来，还会让系统更
     * 倾向把它选成默认网络（Android 里以太网优先级高过 WiFi）。我们并不需要系统
     * 给面子 —— 绑定是逐 socket 做的（bindSocket / SO_BINDTODEVICE），
     * registerNetworkCallback 是纯旁听，不影响选路。
     */
    private void watchEthernet() {
        ConnectivityManager cm = connectivity();
        if (cm == null || netCb != null) return;
        netCb = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                onRadio(RadioLink.this::openUdp);
            }
        };
        try {
            NetworkRequest.Builder b = new NetworkRequest.Builder()
                    .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET);
            if (Build.VERSION.SDK_INT >= 31) {
                b.addTransportType(NetworkCapabilities.TRANSPORT_USB);
            }
            cm.registerNetworkCallback(b.build(), netCb);
        } catch (Exception e) {
            Log.w(TAG, "watchEthernet", e);
        }
    }

    /**
     * 一直盯着 WiFi，好在它换 AP、重连之后重新钉一次 —— Network 对象会变，
     * 钉着旧的那个就等于把整个 App 关进一张死网络里，MESH 再也连不上。
     */
    private void watchWifi() {
        ConnectivityManager cm = connectivity();
        if (cm == null || wifiCb != null) return;
        wifiCb = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                onRadio(RadioLink.this::pinProcess);
            }

            @Override
            public void onLost(Network network) {
                onRadio(RadioLink.this::pinProcess);
            }
        };
        try {
            cm.registerNetworkCallback(new NetworkRequest.Builder()
                    .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                    .build(), wifiCb);
        } catch (Exception e) {
            Log.w(TAG, "watchWifi", e);
        }
        onRadio(this::pinProcess);
    }

    /**
     * 把整个进程的默认出口钉在 WiFi 上。
     *
     * G20 的图传网卡一上线，Android 按以太网看待它、优先级高过 WiFi，系统默认路由
     * 就整个搬过去。现场实测：`wlan0` 是 192.168.1.48、`ar_net0` 是 192.168.1.11，
     * 两张卡同一个网段，`ip route` 里是 `default via 192.168.1.1 dev ar_net0`，
     * 连 `ip route get <网关地址>` 都返回 ar_net0。于是 WebView 里的 WebSocket、
     * WebRTC 全从图传口出去；切到 MESH 时射频是关的（见 G20Rc.setBackupRadio），
     * 那条路发不出去，MESH 整条链路直接断掉，而 WiFi 图标还显示"已连接"。
     *
     * 2.4G 要用的 socket 各自绑了网卡（UDP 在 openSocket，RTSP 在 NativeVideo），
     * 逐 socket 的绑定优先于进程级，所以钉 WiFi 不碍事。但万一两种绑法都没成，
     * 2.4G 就只能靠"系统默认网络正好是图传口"通着 —— 这时候绝不能钉，
     * 控制链路比 MESH 重要。
     */
    private synchronized void pinProcess() {
        ConnectivityManager cm = connectivity();
        if (cm == null) return;
        Network want = (enabled && !udpBound) ? null : findWifi();
        if (want == null ? pinnedNet == null : want.equals(pinnedNet)) return;
        try {
            cm.bindProcessToNetwork(want);
            pinnedNet = want;
            Log.i(TAG, "pin " + want);
        } catch (Exception e) {
            Log.w(TAG, "pin", e);
        }
    }

    @Nullable
    private Network findWifi() {
        ConnectivityManager cm = connectivity();
        if (cm == null) return null;
        try {
            Network[] all = cm.getAllNetworks();
            if (all == null) return null;
            for (Network n : all) {
                NetworkCapabilities c = cm.getNetworkCapabilities(n);
                // WiFi Direct / Aware 那几张也是 TRANSPORT_WIFI，但没有 INTERNET。
                if (c != null
                        && c.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)
                        && c.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)) {
                    return n;
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "findWifi", e);
        }
        return null;
    }

    private static final class AirIf {
        final String name;
        final InetAddress addr;
        final int prefix;
        AirIf(String name, InetAddress addr, int prefix) {
            this.name = name;
            this.addr = addr;
            this.prefix = prefix;
        }
    }

    @Nullable
    private AirIf findAirIf() {
        AirIf n144 = null;
        AirIf wired1 = null;
        try {
            Enumeration<NetworkInterface> en = NetworkInterface.getNetworkInterfaces();
            if (en == null) return null;
            while (en.hasMoreElements()) {
                NetworkInterface ni = en.nextElement();
                if (!ni.isUp() || ni.isLoopback()) continue;
                if (isWifiName(ni.getName())) continue;
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress a = ia.getAddress();
                    if (!(a instanceof Inet4Address)) continue;
                    byte[] v = a.getAddress();
                    int a0 = v[0] & 0xff;
                    int a1 = v[1] & 0xff;
                    int a2 = v[2] & 0xff;
                    AirIf cur = new AirIf(ni.getName(), a, ia.getNetworkPrefixLength());
                    if (a0 == 192 && a1 == 168 && a2 == 144) n144 = cur;
                    else if (a0 == 192 && a1 == 168 && a2 == 1) wired1 = cur;
                }
            }
        } catch (Exception ignored) {
        }
        return n144 != null ? n144 : wired1;
    }

    private static boolean isWifiName(@Nullable String name) {
        if (name == null) return false;
        String n = name.toLowerCase();
        return n.startsWith("wlan") || n.startsWith("wifi") || n.startsWith("ap")
                || n.startsWith("p2p");
    }

    private static String osIfaces() {
        StringBuilder sb = new StringBuilder();
        try {
            Enumeration<NetworkInterface> en = NetworkInterface.getNetworkInterfaces();
            if (en == null) return "";
            while (en.hasMoreElements()) {
                NetworkInterface ni = en.nextElement();
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress a = ia.getAddress();
                    if (!(a instanceof Inet4Address)) continue;
                    if (sb.length() > 0) sb.append(',');
                    sb.append(ni.getName()).append('=').append(a.getHostAddress())
                            .append('/').append(ia.getNetworkPrefixLength());
                }
            }
        } catch (Exception ignored) {
        }
        return sb.toString();
    }

    @Nullable
    private Network findAirlink() {
        ConnectivityManager cm = connectivity();
        if (cm == null) return null;
        Network n144 = null;
        Network wired = null;
        Network lan1 = null;
        for (Network n : cm.getAllNetworks()) {
            LinkProperties lp = cm.getLinkProperties(n);
            if (lp == null) continue;
            NetworkCapabilities caps = cm.getNetworkCapabilities(n);
            if (hasOctets(lp, 192, 168, 144)) n144 = n;
            if (isWired(caps) && hasLanV4(lp)) wired = n;
            if (hasOctets(lp, 192, 168, 1) && isWired(caps)) lan1 = n;
        }
        // G20 USB 通常是 192.168.144.x；有的桥接会直接出现 192.168.1.x。
        if (n144 != null) return n144;
        if (lan1 != null) return lan1;
        return wired;
    }

    private String airLocalIp() {
        ConnectivityManager cm = connectivity();
        if (cm == null || airNet == null) return "";
        return firstV4(cm.getLinkProperties(airNet));
    }

    private String allV4() {
        ConnectivityManager cm = connectivity();
        if (cm == null) return "";
        StringBuilder sb = new StringBuilder();
        for (Network n : cm.getAllNetworks()) {
            String ip = firstV4(cm.getLinkProperties(n));
            if (ip.isEmpty()) continue;
            if (sb.length() > 0) sb.append(',');
            sb.append(ip);
        }
        return sb.toString();
    }

    private static String firstV4(@Nullable LinkProperties lp) {
        if (lp == null) return "";
        for (LinkAddress la : lp.getLinkAddresses()) {
            InetAddress a = la.getAddress();
            if (a instanceof Inet4Address && !a.isLoopbackAddress()) {
                return a.getHostAddress();
            }
        }
        return "";
    }

    @Nullable
    private ConnectivityManager connectivity() {
        if (appCtx == null) return null;
        return (ConnectivityManager) appCtx.getSystemService(Context.CONNECTIVITY_SERVICE);
    }

    private static boolean isWired(@Nullable NetworkCapabilities caps) {
        if (caps == null) return false;
        if (caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)) return true;
        if (Build.VERSION.SDK_INT >= 31
                && caps.hasTransport(NetworkCapabilities.TRANSPORT_USB)) {
            return true;
        }
        return false;
    }

    private static boolean hasOctets(LinkProperties lp, int a, int b, int c) {
        for (LinkAddress la : lp.getLinkAddresses()) {
            InetAddress addr = la.getAddress();
            if (!(addr instanceof Inet4Address) || addr.isLoopbackAddress()) continue;
            byte[] v = addr.getAddress();
            if ((v[0] & 0xff) == a && (v[1] & 0xff) == b && (v[2] & 0xff) == c) {
                return true;
            }
        }
        return false;
    }

    private static boolean hasLanV4(LinkProperties lp) {
        for (LinkAddress la : lp.getLinkAddresses()) {
            InetAddress a = la.getAddress();
            if (!(a instanceof Inet4Address) || a.isLoopbackAddress()) continue;
            byte[] b = a.getAddress();
            int a0 = b[0] & 0xff;
            int a1 = b[1] & 0xff;
            if (a0 == 192 && a1 == 168) return true;
            if (a0 == 10) return true;
            if (a0 == 172 && a1 >= 16 && a1 <= 31) return true;
        }
        return false;
    }

    private void closeSocketOnly() {
        airNet = null;
        boundLocal = null;
        robotAddr = null;
        ifaceName = "";
        ifacePrefix = 0;
        udpBound = false;
        if (udp != null) {
            udp.close();
            udp = null;
        }
    }

    private synchronized void stop() {
        running = false;
        if (handler != null) handler.removeCallbacks(loop);
        cancelPendingStand();
        standing = false;
        // 关掉这一档之后，本机记的姿态就只是历史了：狗可能被 MESH 那侧或原厂手柄
        // 动过。下次进 2.4G 由切档那一刻交接过来（adoptPosture），或者等遥测。
        poseKnown = false;
        telemHeightSeen = false;
        clearWalk();
        confirmed = false;
        buttonsPrimed = false;
        status = "off";
        closeSdkUdp();
        closeSocketOnly();
        ConnectivityManager cm = connectivity();
        if (cm != null && netCb != null) {
            try {
                cm.unregisterNetworkCallback(netCb);
            } catch (Exception ignored) {
            }
            netCb = null;
        }
        // 回 MESH 了，图传口那边射频也关了（G20Rc.setBackupRadio），
        // 这时候必须把出口钉回 WiFi，否则网关在系统默认路由那侧根本发不出去。
        pinProcess();
    }

    private synchronized void onTick() {
        if (!running || !enabled) return;
        handler.postDelayed(loop, TICK_MS);
        if (tick % 50 == 0 && udp == null) openUdp();
        G20Rc.Snapshot snap = G20Rc.get().snapshot();
        if (tick % HB_EVERY == 0) {
            sendSimple(HEARTBEAT);
            if (!confirmed || tick % 250 == 0) {
                sendSimple(CONNECT);
                confirmed = true;
            }
        }
        drainRx();
        tick++;
        if (snap.ch != null && snap.ch.length > 0) {
            handleButtons(snap.ch);
        }
        float[] ax = effAxes(snap.ch);
        if (axesApply()) sendAxes(ax);
    }

    /**
     * 起立 / 趴下 / 急停这三颗在这里读。它们发的指令重复一次也无害（都是幂等的），
     * 所以本机和网页那层各读一遍不要紧 —— 网页那层还要负责出声和刷界面。
     *
     * B1/B2（力控、起步）**不在这里**：踏步是切换指令，本机和网页各发一条就等于
     * 一按一停，狗刚起步又停下。那两颗统一由 web/gamepad.js 的 G20_BTN 派发，
     * 2.4G 下它转头调回本机的 command()，MESH 下走网关，两条链路都只发一次。
     */
    private void handleButtons(int[] ch) {
        if (ch.length <= 12 || !rcLive(ch)) return;
        boolean stand = pressed(ch, 10, 1050);
        boolean sit = pressed(ch, 6, 1050);
        boolean estop = pressed(ch, 12, 1050);
        // 首帧只记档。上电通道常是 0，会被当成急停按下，狗会动一下并锁关节。
        if (!buttonsPrimed) {
            prevStand = stand;
            prevSit = sit;
            prevEstop = estop;
            buttonsPrimed = true;
            return;
        }
        if (stand && !prevStand) commandOnRadio("stand_up");
        if (sit && !prevSit) commandOnRadio("sit_down");
        if (estop && !prevEstop) commandOnRadio("estop");
        prevStand = stand;
        prevSit = sit;
        prevEstop = estop;
    }

    private static boolean rcLive(int[] ch) {
        int ok = 0;
        int n = Math.min(ch.length, 8);
        for (int i = 0; i < n; i++) {
            if (ch[i] >= 900 && ch[i] <= 2100) ok++;
        }
        return ok >= 4;
    }

    /**
     * 和 gamepad.js 的 pwmPressed 同一套判定：按下值在中位以下就按「低有效」看，
     * 在中位以上按「高有效」。L1/L2/急停按下是 1050，B1/B2 按下是 1950。
     */
    private static boolean pressed(int[] ch, int index, int press) {
        if (index < 0 || index >= ch.length) return false;
        int v = ch[index];
        if (v < 900 || v > 2100) return false;
        int mid = (press + 1500) / 2;
        return press < 1500 ? v <= mid : v >= mid;
    }

    /**
     * 一帧轴。实体摇杆优先，它回中时用屏幕摇杆那份 —— 平板上那两个虚拟摇杆以前
     * 推了没反应：桥接方法在，但 setScreenAxes 是个空壳。
     *
     * 屏幕那份超过 SCREEN_AXIS_HOLD_MS 没更新就按松手算（发 0）。页面卡住或切后台
     * 时不能让狗照着最后一帧一直走。
     */
    /**
     * 摇杆现在指哪，把实体手柄和屏幕两个来源合成一份，方向已经是协议轴的方向。
     * 实体优先：手上那根杆比屏幕上的可信，也更快。
     */
    private float[] effAxes(@Nullable int[] ch) {
        float ly = 0f;
        float lx = 0f;
        float rx = 0f;
        float ry = 0f;
        // 通道要先确认是活的。上电时通道常是全 0，按 axis() 的换算 (0-1500)/500
        // 会被读成满量程后退。力控/起步之后才发轴，这一脚会直接踹出去。
        if (ch != null && ch.length > 0 && rcLive(ch)) {
            ly = axis(ch, 2, false);
            lx = -axis(ch, 3, true);
            rx = -axis(ch, 0, true);
            ry = axis(ch, 1, false);
        }
        if (ly == 0f && lx == 0f && rx == 0f && ry == 0f
                && System.currentTimeMillis() - scrAt < SCREEN_AXIS_HOLD_MS) {
            ly = scrFwd;
            lx = -scrLat;
            rx = -scrTurn;
            ry = scrTilt;
        }
        axBuf[0] = ly;
        axBuf[1] = lx;
        axBuf[2] = rx;
        axBuf[3] = stepping ? 0f : ry;
        return axBuf;
    }

    private void sendAxes(float[] a) {
        sendSimple(AXIS_LY, bits(a[0]));
        sendSimple(AXIS_LX, bits(a[1]));
        sendSimple(AXIS_RX, bits(a[2]));
        if (!stepping) sendSimple(AXIS_RY, bits(a.length > 3 ? a[3] : 0f));
    }

    private static float axis(int[] ch, int index, boolean invert) {
        if (index < 0 || index >= ch.length) return 0f;
        float n = (ch[index] - 1500f) / 500f;
        if (n > 1f) n = 1f;
        if (n < -1f) n = -1f;
        if (Math.abs(n) < 0.12f) n = 0f;
        return invert ? -n : n;
    }

    private static int bits(float v) {
        if (v > 1f) v = 1f;
        if (v < -1f) v = -1f;
        int raw = (int) (v * AXIS_MAX);
        if (Math.abs(raw) < AXIS_DZ) raw = 0;
        if (raw > AXIS_MAX) raw = AXIS_MAX;
        if (raw < -AXIS_MAX) raw = -AXIS_MAX;
        return raw;
    }

    private void sendSimple(int code) {
        sendSimple(code, 0);
    }

    private synchronized void sendSimple(int code, int value) {
        if (code == 0) return;
        ByteBuffer buf = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(code);
        buf.putInt(value);
        buf.putInt(0);
        byte[] pkt = buf.array();
        // 只走一条成功路径。起步/力控是切换指令，多路齐发会互相抵消。
        if (udp != null && sendUdp(pkt, robotAddr)) {
            sentOk++;
            return;
        }
        sentFail++;
    }

    private static boolean writePipe(Pipeline pipe, byte[] pkt) {
        try {
            pipe.writeData(pkt);
            return true;
        } catch (Throwable t) {
            Log.w(TAG, "pipe send", t);
            return false;
        }
    }

    private void drainRx() {
        if (udp == null) return;
        byte[] buf = new byte[256];
        try {
            while (true) {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                udp.receive(p);
                rxOk++;
                readTelem(buf, p.getLength());
            }
        } catch (java.net.SocketTimeoutException ignored) {
        } catch (Exception e) {
            if (rxOk == 0 && lastErr.isEmpty()) lastErr = "rx:" + brief(e);
        }
    }

    /**
     * 读运动主机的遥测。只取姿态那一包（0x1009）：头 12 字节之后第一个字节是
     * basic_state，第二个是 gait_state，与 rk3588 那侧解的是同一份结构。
     *
     * 这几个字段一到手，本地那些猜测（站没站、力控没力控）就能自己纠回来 ——
     * 以前 2.4G 只靠「我发过什么」记状态，别的遥控器动过狗、或者刚从 MESH 切回来，
     * 记的和实际就是两回事。
     */
    private void readTelem(byte[] b, int len) {
        if (len < HEAD_LEN) return;
        int code = (b[0] & 0xff) | ((b[1] & 0xff) << 8)
                | ((b[2] & 0xff) << 16) | ((b[3] & 0xff) << 24);
        // 身高档位这条是简单报文，只有 12 字节的头，档位就在头里，不带 data。
        if (code == TELEM_HEIGHT) {
            telemHeight = (b[4] & 0xff) | ((b[5] & 0xff) << 8)
                    | ((b[6] & 0xff) << 16) | ((b[7] & 0xff) << 24);
            telemHeightSeen = true;
            return;
        }
        if (code == TELEM_RUNNING && len >= HEAD_LEN + RCS_LEN) {
            telemNavMode = b[HEAD_LEN + RCS_NAV_OFF] & 0xff;
            telemEmergSrc = b[HEAD_LEN + RCS_EMERG_OFF] & 0xff;
            telemRunSeen = true;
            telemAt = System.currentTimeMillis();
            applyTelemLock();
            return;
        }
        if (code != TELEM_MOTION || len < HEAD_LEN + 2) return;
        telemState = b[HEAD_LEN] & 0xff;
        telemGait = b[HEAD_LEN + 1] & 0xff;
        telemAt = System.currentTimeMillis();
        if (telemUpright() && telemEmergSrc == 0) {
            standing = true;
            emergency = false;
            // 力控站立 / 踏步是主机说的，比本地这两个标志准。但初始站立不要
            // 把刚点的力控/起步清掉：RL 起立后常停在 2，清了再按起步等于停步。
            if (telemState == ST_STEPPING) {
                torqued = true;
                // 主机自己踏步不要当成「人按了起步」，否则停踏步会直接 return。
            } else if (telemState == ST_TORQUE_STANDING && !stepping) {
                torqued = true;
            }
        } else if (telemState == ST_EMERGENCY
                || (telemEmergSrc >= 4 && telemEmergSrc <= 6)) {
            applyTelemLock();
        } else if (telemState == ST_SITTING) {
            // RL 起立后主机仍报坐下。分不清真趴着还是那种谎报，不动 standing。
            // 急停后也常报坐下，但关节锁着 —— 有 0x1008 才敢把急停旗标清掉。
            if (telemRunSeen && telemEmergSrc == 0) emergency = false;
        } else if (telemState == ST_STAND_TO_SIT) {
            clearWalk();
        }
    }

    private void applyTelemLock() {
        if (telemState != ST_EMERGENCY
                && !(telemEmergSrc >= 4 && telemEmergSrc <= 6)) {
            return;
        }
        emergency = true;
        standing = false;
        clearWalk();
    }

    private boolean sendUdp(byte[] pkt, @Nullable InetAddress dest) {
        if (dest == null || udp == null) {
            if (dest == null) lastErr = "no-dest";
            return false;
        }
        try {
            udp.send(new DatagramPacket(pkt, pkt.length, dest, ROBOT_PORT));
            if (!lastErr.isEmpty()) lastErr = "";
            return true;
        } catch (Exception e) {
            lastErr = brief(e);
            Log.w(TAG, "udp send " + dest, e);
            return false;
        }
    }

    private static String brief(Throwable e) {
        String m = e.getMessage();
        if (m == null || m.isEmpty()) return e.getClass().getSimpleName();
        return e.getClass().getSimpleName() + ":" + m;
    }

    private static void bindToDevice(DatagramSocket sock, String ifname) throws Exception {
        bindFd(fdOf(sock), ifname);
    }

    /** TCP 版。2.4G 下机身相机的 RTSP 走这条，见 NativeVideo。 */
    static void bindToDevice(Socket sock, String ifname) throws Exception {
        bindFd(fdOf(sock), ifname);
    }

    private static void bindFd(FileDescriptor fd, String ifname) throws Exception {
        int opt = 25;
        try {
            Field f = OsConstants.class.getField("SO_BINDTODEVICE");
            opt = f.getInt(null);
        } catch (Throwable ignored) {
        }
        // setsockoptIfreq 是 @hide，公开 SDK 没有这个符号，只能反射。
        try {
            Method m = Os.class.getMethod(
                    "setsockoptIfreq", FileDescriptor.class, int.class, int.class, String.class);
            m.invoke(null, fd, OsConstants.SOL_SOCKET, opt, ifname);
        } catch (java.lang.reflect.InvocationTargetException e) {
            Throwable c = e.getCause();
            if (c instanceof Exception) throw (Exception) c;
            if (c instanceof Error) throw (Error) c;
            throw e;
        }
        Log.i(TAG, "bind " + ifname);
    }

    private static FileDescriptor fdOf(DatagramSocket sock) throws Exception {
        try {
            Method m = DatagramSocket.class.getDeclaredMethod("getFileDescriptor$");
            m.setAccessible(true);
            FileDescriptor fd = (FileDescriptor) m.invoke(sock);
            if (fd != null) return fd;
        } catch (Throwable ignored) {
        }
        Field implF = DatagramSocket.class.getDeclaredField("impl");
        implF.setAccessible(true);
        Object impl = implF.get(sock);
        FileDescriptor fd = (FileDescriptor) invokeUp(impl, "getFileDescriptor");
        if (fd == null) throw new IllegalStateException("no-fd");
        return fd;
    }

    private static FileDescriptor fdOf(Socket sock) throws Exception {
        try {
            Method m = Socket.class.getDeclaredMethod("getFileDescriptor$");
            m.setAccessible(true);
            FileDescriptor fd = (FileDescriptor) m.invoke(sock);
            // 没 bind 也没 connect 过的 socket 还没有 fd，这里会拿到个无效的。
            if (fd != null && fd.valid()) return fd;
        } catch (Throwable ignored) {
        }
        Field implF = Socket.class.getDeclaredField("impl");
        implF.setAccessible(true);
        Object impl = implF.get(sock);
        if (impl == null) throw new IllegalStateException("no-impl");
        FileDescriptor fd = (FileDescriptor) invokeUp(impl, "getFileDescriptor");
        if (fd == null) throw new IllegalStateException("no-fd");
        return fd;
    }

    /** getFileDescriptor 声明在 SocketImpl 上，实际对象往往是它的子类，得往上找。 */
    private static Object invokeUp(Object target, String name) throws Exception {
        for (Class<?> c = target.getClass(); c != null; c = c.getSuperclass()) {
            try {
                Method m = c.getDeclaredMethod(name);
                m.setAccessible(true);
                return m.invoke(target);
            } catch (NoSuchMethodException ignored) {
            }
        }
        throw new NoSuchMethodException(name);
    }
}
