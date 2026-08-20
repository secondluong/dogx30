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
    private static final int AXIS_MAX = 32767;
    private static final int AXIS_DZ = 655;

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
    private boolean torqued;
    private boolean stepping;
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

    private final Runnable loop = this::onTick;

    synchronized void attach(Context ctx) {
        if (ctx != null) appCtx = ctx.getApplicationContext();
        ensureWorker();
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

    synchronized boolean isStanding() {
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
            o.put("standing", standing);
            return o.toString();
        } catch (Exception e) {
            return "{}";
        }
    }

    /** 钉在顶栏，不用翻黄条。 */
    synchronized String statusLine() {
        StringBuilder sb = new StringBuilder();
        sb.append(isLinkReady() ? "2.4G 已通 " : "2.4G 未通 ");
        sb.append(status);
        String local = boundLocal != null ? boundLocal.getHostAddress() : airLocalIp();
        if (!local.isEmpty()) sb.append(" 本机").append(local);
        String ifaces = osIfaces();
        if (!ifaces.isEmpty()) sb.append(" ").append(ifaces);
        if (!lastErr.isEmpty() && sentOk == 0) sb.append(" ").append(lastErr);
        sb.append(" ok").append(sentOk).append("/fail").append(sentFail);
        sb.append(" rx").append(rxOk);
        if (!lastCmd.isEmpty()) sb.append(" ").append(lastCmd);
        return sb.toString();
    }

    synchronized void setScreenAxes(float fwd, float lat, float turn) {
    }

    private void clearWalk() {
        torqued = false;
        stepping = false;
    }

    void command(String name) {
        if (name == null) return;
        onRadio(() -> commandOnRadio(name));
    }

    private synchronized void commandOnRadio(String name) {
        lastCmd = name;
        if (!enabled) {
            enabled = true;
            start();
        }
        if (udp == null) openUdp();
        switch (name) {
            case "stand_up":
                if (emergency) {
                    sendSimple(UNLOAD);
                    emergency = false;
                    standing = false;
                    clearWalk();
                } else {
                    sendSimple(STAND);
                    standing = true;
                    clearWalk();
                }
                break;
            case "sit":
            case "sit_down":
                sendSimple(SIT);
                standing = false;
                clearWalk();
                break;
            case "stand":
                if (emergency) {
                    sendSimple(UNLOAD);
                    emergency = false;
                    standing = false;
                    clearWalk();
                } else if (standing) {
                    sendSimple(SIT);
                    standing = false;
                    clearWalk();
                } else {
                    sendSimple(STAND);
                    standing = true;
                    clearWalk();
                }
                break;
            case "unload":
                sendSimple(UNLOAD);
                emergency = false;
                standing = false;
                clearWalk();
                break;
            case "torque":
                if (stepping) {
                    sendSimple(STEP);
                    stepping = false;
                } else {
                    sendSimple(TORQUE);
                }
                torqued = true;
                break;
            case "step":
                if (!standing) break;
                if (!torqued) {
                    sendSimple(TORQUE);
                    torqued = true;
                }
                if (!stepping) {
                    sendSimple(STEP);
                    stepping = true;
                }
                break;
            case "estop":
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
                    sendSimple(gaitCode(name.substring(5)), 0);
                } else if (gaitCode(name) != 0) {
                    sendSimple(gaitCode(name), 0);
                }
                break;
        }
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
        requestEthernet();
        openUdp();
        if (!running) {
            running = true;
            handler.post(loop);
        }
    }

    private synchronized void openUdp() {
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
            ConnectivityManager cm = connectivity();
            if (cm != null && net != null) {
                cm.bindProcessToNetwork(net);
            }
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
                } catch (Exception e) {
                    Log.w(TAG, "bindSocket", e);
                }
            } else if (!name.isEmpty()) {
                try {
                    bindToDevice(udp, name);
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

    private void requestEthernet() {
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
            cm.requestNetwork(b.build(), netCb);
        } catch (Exception e) {
            Log.w(TAG, "requestNetwork", e);
        }
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
        if (udp != null) {
            udp.close();
            udp = null;
        }
        ConnectivityManager cm = connectivity();
        if (cm != null) {
            try {
                cm.bindProcessToNetwork(null);
            } catch (Exception ignored) {
            }
        }
    }

    private synchronized void stop() {
        running = false;
        if (handler != null) handler.removeCallbacks(loop);
        standing = false;
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
        if (torqued || stepping) {
            if (snap.ch != null && snap.ch.length > 0) {
                sendAxes(snap.ch);
            } else if (stepping) {
                sendSimple(AXIS_LY, 0);
                sendSimple(AXIS_LX, 0);
                sendSimple(AXIS_RX, 0);
            }
        }
    }

    private void handleButtons(int[] ch) {
        if (ch.length <= 12 || !rcLive(ch)) return;
        boolean stand = pressed(ch, 10);
        boolean sit = pressed(ch, 6);
        boolean estop = pressed(ch, 12);
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

    private static boolean pressed(int[] ch, int index) {
        if (index < 0 || index >= ch.length) return false;
        int v = ch[index];
        if (v < 900 || v > 2100) return false;
        return v <= 1275;
    }

    private void sendAxes(int[] ch) {
        float fwd = axis(ch, 2, false);
        float lat = axis(ch, 3, true);
        float turn = axis(ch, 0, true);
        sendSimple(AXIS_LY, bits(fwd));
        sendSimple(AXIS_LX, bits(-lat));
        sendSimple(AXIS_RX, bits(-turn));
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
            }
        } catch (java.net.SocketTimeoutException ignored) {
        } catch (Exception e) {
            if (rxOk == 0 && lastErr.isEmpty()) lastErr = "rx:" + brief(e);
        }
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
        FileDescriptor fd = fdOf(sock);
        int opt = 25;
        try {
            Field f = OsConstants.class.getField("SO_BINDTODEVICE");
            opt = f.getInt(null);
        } catch (Throwable ignored) {
        }
        Os.setsockoptIfreq(fd, OsConstants.SOL_SOCKET, opt, ifname);
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
        Method getFd = impl.getClass().getDeclaredMethod("getFileDescriptor");
        getFd.setAccessible(true);
        FileDescriptor fd = (FileDescriptor) getFd.invoke(impl);
        if (fd == null) throw new IllegalStateException("no-fd");
        return fd;
    }
}
