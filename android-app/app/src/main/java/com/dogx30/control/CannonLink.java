package com.dogx30.control;

import android.content.Context;
import android.net.Network;
import android.util.Log;

import androidx.annotation.Nullable;

import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;

/**
 * 消防炮在 1 网 192.168.1.253:4000。平板和运动 UDP 用同一张 2.4G 网卡。
 * 现场从 192.168.1.11 直连炮台完不成握手，炮和狗在同一台交换机上，
 * 所以直连失败后改连网关 192.168.1.120:4001，由网关 eth0 去连炮。
 * 帧仍是 13 字节，开阀才出水。
 */
final class CannonLink {
    private static final String TAG = "CannonLink";
    private static final String HOST = "192.168.1.253";
    private static final int PORT = 4000;
    /** 网关 eth0。平板到得了狗（.103/.106），到不了炮时走这里。 */
    private static final String RELAY_HOST = "192.168.1.120";
    private static final int RELAY_PORT = 4001;
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
    private String hop = "";
    private String relay = "";
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
                    + ",\"via\":\"" + via + "\""
                    + ",\"hop\":\"" + hop + "\""
                    + ",\"relay\":\"" + relay + "\"}";
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
     * 和机身监控同一条绑法：有 Network 就 bindSocket，否则 SO_BINDTODEVICE。
     * 先试炮台。192.168.1.11 直连 .253 在现场完不成握手，再试网关转发口。
     * 后一次拨号若是本地绑口失败，保留前一次的网络错误，避免横幅被盖成「连不上」。
     */
    private Socket connect() {
        Radio radio = radio();
        if (radio == null) {
            setOnline(false, "wait", "");
            return null;
        }
        String src = radio.addr != null ? radio.addr.getHostAddress() : radio.name;
        Socket direct = open(radio, HOST, PORT);
        if (direct != null) {
            markUp("direct", src);
            Log.i(TAG, "水炮已连 " + HOST + ":" + PORT + " via " + src);
            return direct;
        }
        String directWhy = lastOpenErr == null ? "other" : reason(lastOpenErr);
        Socket gw = open(radio, RELAY_HOST, RELAY_PORT);
        if (gw != null) {
            markUp("gw", src);
            Log.i(TAG, "水炮经网关 " + RELAY_HOST + ":" + RELAY_PORT + " via " + src);
            return gw;
        }
        String relayWhy = lastOpenErr == null ? "other" : reason(lastOpenErr);
        setFail(directWhy, relayWhy, src);
        Log.w(TAG, "cannon " + directWhy + " relay " + relayWhy + " via " + src);
        return null;
    }

    private Exception lastOpenErr;

    @Nullable
    private Socket open(Radio radio, String host, int port) {
        Exception first = null;
        Socket s = dial(radio, host, port, false);
        if (s != null) return s;
        first = lastOpenErr;
        if (radio.addr != null) {
            s = dial(radio, host, port, true);
            if (s != null) return s;
            if (first != null && localProblem(lastOpenErr)) lastOpenErr = first;
        }
        return null;
    }

    private Socket dial(Radio radio, String host, int port, boolean bindLocal) {
        Socket s = new Socket();
        lastOpenErr = null;
        try {
            if (bindLocal && radio.addr != null) {
                s.bind(new InetSocketAddress(radio.addr, 0));
            }
            if (!bindLocal && radio.net != null) radio.net.bindSocket(s);
            else if (radio.name != null && !radio.name.isEmpty()) {
                RadioLink.bindToDevice(s, radio.name);
            }
            s.connect(new InetSocketAddress(InetAddress.getByName(host), port), 1000);
            s.setTcpNoDelay(true);
            s.setSoTimeout(40);
            return s;
        } catch (Exception e) {
            lastOpenErr = e;
            Log.w(TAG, "connect " + host + ":" + port
                    + (bindLocal ? " bind " : " ")
                    + (radio.addr != null ? radio.addr.getHostAddress() : radio.name), e);
            close(s);
            return null;
        }
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

    private void markUp(String which, String src) {
        synchronized (lock) {
            online = true;
            status = "tcp";
            err = "";
            hop = which == null ? "" : which;
            via = src == null ? "" : src;
            relay = "gw".equals(which) ? "ok" : "";
        }
    }

    private void setFail(String error, String relayState, String src) {
        synchronized (lock) {
            online = false;
            status = "tcp-fail";
            err = error == null ? "" : error;
            relay = relayState == null ? "" : relayState;
            hop = "";
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
                hop = "";
                relay = "";
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
