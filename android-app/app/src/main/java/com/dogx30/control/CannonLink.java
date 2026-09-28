package com.dogx30.control;

import android.content.Context;
import android.net.Network;
import android.util.Log;

import androidx.annotation.Nullable;

import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;

/**
 * 消防炮在 1 网 192.168.1.253:4000。2.4G 下平板和运动 UDP 用同一张射频网卡，
 * 直接做炮台的 TCP 客户端，不经网关、不进 MESH。
 * 绑口顺序和机身相机一样：系统看得见这张网就 bindSocket，否则先 bind 本机地址
 * （这一步才有 fd）再 SO_BINDTODEVICE。帧是 13 字节，开阀才出水。
 */
final class CannonLink {
    private static final String TAG = "CannonLink";
    private static final String HOST = "192.168.1.253";
    private static final int PORT = 4000;
    private static final int PERIOD_MS = 100;
    private static final int STALE_MS = 800;
    private static final float DEAD = 0.08f;
    private static final byte[] HDR = {0x08, 0x00, 0x00, 0x00, 0x08};

    private static final CannonLink INST = new CannonLink();

    static CannonLink get() {
        return INST;
    }

    private Context appCtx;
    private final Object lock = new Object();
    private float pan;
    private float tilt;
    private String spray = "";
    private boolean fire;
    private long touchAt;
    private boolean online;
    private boolean havePressure;
    private float pressureMpa;
    private String status = "idle";
    private String err = "";
    private String via = "";
    private Thread thread;
    private volatile boolean running;

    void attach(Context ctx) {
        if (ctx != null) appCtx = ctx.getApplicationContext();
    }

    void set(float pan, float tilt, String spray, boolean fire) {
        synchronized (lock) {
            this.pan = pan;
            this.tilt = tilt;
            this.spray = spray == null ? "" : spray;
            this.fire = fire;
            this.touchAt = System.currentTimeMillis();
        }
        ensure();
    }

    String statusJson() {
        synchronized (lock) {
            String sp = spray.isEmpty() ? "off" : spray;
            return "{\"online\":" + online
                    + ",\"have_pressure\":" + havePressure
                    + ",\"pressure_mpa\":" + trim(pressureMpa)
                    + ",\"fire\":" + fire
                    + ",\"spray\":\"" + sp + "\""
                    + ",\"status\":\"" + status + "\""
                    + ",\"err\":\"" + err + "\""
                    + ",\"via\":\"" + via + "\"}";
        }
    }

    private void ensure() {
        if (running) return;
        running = true;
        thread = new Thread(this::loop, "cannon-24");
        thread.start();
    }

    private void loop() {
        Socket sock = null;
        boolean sentIdle = true;
        byte[] rx = new byte[64];
        int rxLen = 0;
        while (running) {
            Cmd cmd = snapshot();
            long now = System.currentTimeMillis();
            if (now - cmd.touchAt > STALE_MS) {
                if (sock != null && !sentIdle) {
                    sendFrame(sock, frame(new Cmd()));
                    sentIdle = true;
                }
                close(sock);
                sock = null;
                setOnline(false, "idle");
                sleep(PERIOD_MS);
                continue;
            }
            if (sock == null || sock.isClosed()) {
                sock = connect();
                sentIdle = false;
                rxLen = 0;
            }
            byte[] frame = frame(cmd);
            boolean idle = idleFrame(frame);
            if (sock != null && (!idle || !sentIdle)) {
                if (sendFrame(sock, frame)) sentIdle = idle;
                else {
                    close(sock);
                    sock = null;
                }
            }
            if (sock != null) rxLen = drain(sock, rx, rxLen);
            sleep(PERIOD_MS);
        }
        if (sock != null) {
            sendFrame(sock, frame(new Cmd()));
            close(sock);
        }
    }

    /**
     * 只连炮台。系统看得见射频网卡时用 bindSocket；看不见（ar_net0 常见）
     * 则先 bind 本机地址再 SO_BINDTODEVICE。后一次若是本地绑口失败，保留
     * 前一次的网络错误，避免横幅被盖成「连不上」。不走默认路由，以免从
     * WiFi 绕进网关。
     */
    private Socket connect() {
        Radio radio = radio();
        if (radio == null) {
            setOnline(false, "wait", "");
            return null;
        }
        String src = radio.addr != null ? radio.addr.getHostAddress() : radio.name;
        Socket s = null;
        if (radio.net != null) s = dial(radio, true);
        else lastOpenErr = null;
        if (s == null) {
            Exception first = lastOpenErr;
            s = dial(radio, false);
            if (s == null && first != null && localProblem(lastOpenErr)) lastOpenErr = first;
        }
        if (s != null) {
            markUp(src);
            Log.i(TAG, "水炮已连 " + HOST + ":" + PORT + " via " + src);
            return s;
        }
        String why = lastOpenErr == null ? "other" : reason(lastOpenErr);
        setFail(why, src);
        Log.w(TAG, "cannon " + why + " via " + src);
        return null;
    }

    private Exception lastOpenErr;

    @Nullable
    private Socket dial(Radio radio, boolean preferNet) {
        Socket s = new Socket();
        lastOpenErr = null;
        try {
            if (preferNet) {
                if (radio.net == null) {
                    close(s);
                    return null;
                }
                radio.net.bindSocket(s);
            } else if (!bindRadio(s, radio)) {
                lastOpenErr = new java.net.SocketException("no-radio");
                close(s);
                return null;
            }
            s.connect(new InetSocketAddress(InetAddress.getByName(HOST), PORT), 1500);
            s.setTcpNoDelay(true);
            s.setSoTimeout(40);
            return s;
        } catch (Exception e) {
            lastOpenErr = e;
            Log.w(TAG, "connect " + HOST + ":" + PORT
                    + (preferNet ? " net " : " dev ")
                    + (radio.addr != null ? radio.addr.getHostAddress() : radio.name), e);
            close(s);
            return null;
        }
    }

    /** 先 bind 本机地址创建 fd，再按网卡名绑。两样都没有就不连。 */
    private static boolean bindRadio(Socket s, Radio radio) throws Exception {
        boolean bound = false;
        if (radio.addr != null) {
            s.bind(new InetSocketAddress(radio.addr, 0));
            bound = true;
        }
        if (radio.name != null && !radio.name.isEmpty()) {
            RadioLink.bindToDevice(s, radio.name);
            bound = true;
        }
        return bound;
    }

    private static boolean localProblem(@Nullable Exception e) {
        if (e == null) return false;
        String m = text(e);
        return m.contains("no-fd") || m.contains("no-impl") || m.contains("bind failed")
                || m.contains("eperm") || m.contains("enodev") || m.contains("eaddrnotavail");
    }

    private static String reason(Exception e) {
        String m = text(e);
        if (m.contains("refused") || m.contains("econnrefused")) return "refused";
        if (m.contains("timeout") || m.contains("timed out") || m.contains("etimedout")
                || m.contains("after ")) return "timeout";
        if (m.contains("unreachable") || m.contains("no route")
                || m.contains("ehostunreach") || m.contains("enetunreach")) return "unreachable";
        return "other";
    }

    private static String text(Throwable e) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; e != null && i < 4; i++, e = e.getCause()) {
            if (e.getMessage() != null) sb.append(e.getMessage()).append(' ');
            sb.append(e.getClass().getSimpleName()).append(' ');
        }
        return sb.toString().toLowerCase();
    }

    @Nullable
    private static Radio radio() {
        RadioLink link = RadioLink.get();
        Network net = link.airNetwork();
        String name = link.ifaceName();
        InetAddress addr = link.localAddress();
        if (net == null && (name == null || name.isEmpty()) && addr == null) return null;
        return new Radio(net, name == null ? "" : name, addr);
    }

    private void markUp(String src) {
        synchronized (lock) {
            online = true;
            status = "tcp";
            err = "";
            via = src == null ? "" : src;
        }
    }

    private void setFail(String error, String src) {
        synchronized (lock) {
            online = false;
            status = "tcp-fail";
            err = error == null ? "" : error;
            via = src == null ? "" : src;
            havePressure = false;
            pressureMpa = 0;
        }
    }

    private static final class Radio {
        final Network net;
        final String name;
        final InetAddress addr;
        Radio(Network net, String name, InetAddress addr) {
            this.net = net;
            this.name = name;
            this.addr = addr;
        }
    }

    private int drain(Socket sock, byte[] rx, int rxLen) {
        try {
            java.io.InputStream in = sock.getInputStream();
            int n = in.read(rx, rxLen, rx.length - rxLen);
            if (n <= 0) return rxLen;
            rxLen += n;
        } catch (java.net.SocketTimeoutException ignored) {
            return rxLen;
        } catch (Exception e) {
            return rxLen;
        }
        int i = 0;
        while (i + 13 <= rxLen) {
            if (rx[i] != 0x08) {
                i++;
                continue;
            }
            if (rx[i + 3] == 0x03) {
                int raw = (rx[i + 5] & 0xff) | ((rx[i + 6] & 0xff) << 8);
                synchronized (lock) {
                    havePressure = true;
                    pressureMpa = raw / 2500f;
                }
            }
            i += 13;
        }
        if (i > 0 && i < rxLen) {
            System.arraycopy(rx, i, rx, 0, rxLen - i);
            rxLen -= i;
        } else if (i >= rxLen) {
            rxLen = 0;
        }
        return rxLen;
    }

    private boolean sendFrame(Socket sock, byte[] frame) {
        try {
            sock.getOutputStream().write(frame);
            sock.getOutputStream().flush();
            return true;
        } catch (Exception e) {
            setOnline(false, "tcp-fail");
            return false;
        }
    }

    private void close(@Nullable Socket sock) {
        if (sock == null) return;
        try {
            sock.close();
        } catch (Exception ignored) {
        }
    }

    private void setOnline(boolean on, String st) {
        setOnline(on, st, "");
    }

    private void setOnline(boolean on, String st, String error) {
        synchronized (lock) {
            online = on;
            status = st;
            err = error == null ? "" : error;
            if (!on) {
                // 断线后旧水压不再显示，避免没连上还留着上一口的数。
                havePressure = false;
                pressureMpa = 0;
            }
        }
    }

    private Cmd snapshot() {
        synchronized (lock) {
            Cmd c = new Cmd();
            c.pan = pan;
            c.tilt = tilt;
            c.spray = spray;
            c.fire = fire;
            c.touchAt = touchAt;
            return c;
        }
    }

    private static byte[] frame(Cmd cmd) {
        byte[] out = new byte[13];
        System.arraycopy(HDR, 0, out, 0, 5);
        int d0 = 0;
        if (Math.abs(cmd.tilt) > DEAD) d0 |= cmd.tilt > 0 ? 0x02 : 0x04;
        if (Math.abs(cmd.pan) > DEAD) d0 |= cmd.pan < 0 ? 0x08 : 0x10;
        if ("fog".equals(cmd.spray)) d0 |= 0x20;
        else if ("jet".equals(cmd.spray)) d0 |= 0x40;
        out[5] = (byte) d0;
        if (cmd.fire) out[7] = 0x02;
        return out;
    }

    private static boolean idleFrame(byte[] frame) {
        for (int i = 5; i < frame.length; i++) {
            if (frame[i] != 0) return false;
        }
        return true;
    }

    private static String trim(float v) {
        return String.format(java.util.Locale.US, "%.3f", v);
    }

    private static void sleep(int ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
    }

    private static final class Cmd {
        float pan;
        float tilt;
        String spray = "";
        boolean fire;
        long touchAt;
    }
}
