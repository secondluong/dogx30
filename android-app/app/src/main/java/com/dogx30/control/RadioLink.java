package com.dogx30.control;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import androidx.annotation.Nullable;

import com.skydroid.rcsdk.PipelineManager;
import com.skydroid.rcsdk.comm.CommListener;
import com.skydroid.rcsdk.common.error.SkyException;
import com.skydroid.rcsdk.common.pipeline.Pipeline;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * 按云卓 RCSDK-Demo 的 G20 数传来发 0x21。
 * 官方写法：createG12G20Pipeline()，以及
 * createUDPPipeline(本地端口, "192.168.144.10", 远端端口)。
 * 助手 / 云深处 App 占着端口时管道会连不上。
 */
final class RadioLink {

    private static final String TAG = "RadioLink";
    private static final String ROBOT_IP = "192.168.1.103";
    private static final String AIR_IP = "192.168.144.10";
    private static final int ROBOT_PORT = 43893;
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

    private final Handler handler = new Handler(Looper.getMainLooper());
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
    private int tick;
    private boolean g20Ok;
    private boolean udpPipeOk;
    @Nullable private Context appCtx;
    @Nullable private DatagramSocket udp;
    @Nullable private InetAddress robotAddr;
    @Nullable private Network airNet;
    @Nullable private Pipeline g20Pipe;
    @Nullable private Pipeline udpPipe;

    private final Runnable loop = this::onTick;

    synchronized void attach(Context ctx) {
        if (ctx != null) appCtx = ctx.getApplicationContext();
    }

    synchronized void setEnabled(boolean on) {
        if (enabled == on) return;
        enabled = on;
        if (on) start();
        else stop();
    }

    synchronized boolean isStanding() {
        return standing;
    }

    synchronized boolean isLinkReady() {
        return enabled && (g20Ok || udpPipeOk || (udp != null && airNet != null));
    }

    synchronized void setScreenAxes(float fwd, float lat, float turn) {
    }

    private void clearWalk() {
        torqued = false;
        stepping = false;
    }

    synchronized void command(String name) {
        if (!enabled || name == null) return;
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

    private void start() {
        confirmed = false;
        tick = 0;
        openPipes();
        openUdp();
        if (!running) {
            running = true;
            handler.post(loop);
        }
    }

    private synchronized void openUdp() {
        closeSocket();
        try {
            Network net = findAirlink();
            if (net == null) {
                Log.w(TAG, "no G20 usb 192.168.144");
                return;
            }
            ConnectivityManager cm = connectivity();
            if (cm != null) {
                cm.bindProcessToNetwork(net);
            }
            airNet = net;
            robotAddr = net.getByName(ROBOT_IP);
            udp = new DatagramSocket();
            net.bindSocket(udp);
            Log.i(TAG, "radio " + net + " -> " + robotAddr);
        } catch (Exception e) {
            Log.w(TAG, "udp", e);
            closeSocket();
        }
    }

    @Nullable
    private Network findAirlink() {
        ConnectivityManager cm = connectivity();
        if (cm == null) return null;
        Network wired = null;
        for (Network n : cm.getAllNetworks()) {
            LinkProperties lp = cm.getLinkProperties(n);
            if (lp == null) continue;
            if (hasOctets(lp, 192, 168, 144)) return n;
            NetworkCapabilities caps = cm.getNetworkCapabilities(n);
            if (isWired(caps) && hasLanV4(lp)) wired = n;
        }
        return wired;
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

    private void closeSocket() {
        airNet = null;
        robotAddr = null;
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

    private void stop() {
        running = false;
        handler.removeCallbacks(loop);
        standing = false;
        clearWalk();
        confirmed = false;
        closePipes();
        closeSocket();
    }

    private void onTick() {
        if (!running || !enabled) return;
        handler.postDelayed(loop, TICK_MS);
        if (tick % 50 == 0) {
            if (!g20Ok && !udpPipeOk) openPipes();
            if (udp == null) openUdp();
        }
        G20Rc.Snapshot snap = G20Rc.get().snapshot();
        if (tick % HB_EVERY == 0) {
            sendSimple(HEARTBEAT);
            if (!confirmed) {
                sendSimple(CONNECT);
                confirmed = true;
            }
        }
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
        boolean stand = pressed(ch, 10);
        boolean sit = pressed(ch, 6);
        boolean estop = pressed(ch, 12);
        if (stand && !prevStand) command("stand_up");
        if (sit && !prevSit) command("sit_down");
        if (estop && !prevEstop) command("estop");
        prevStand = stand;
        prevSit = sit;
        prevEstop = estop;
    }

    private static boolean pressed(int[] ch, int index) {
        if (index < 0 || index >= ch.length) return false;
        return ch[index] <= 1275;
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
        // 官方 Demo：管道 writeData。两条路一起发会把起步切换打两次抵消。
        if (g20Ok && g20Pipe != null) {
            try {
                g20Pipe.writeData(pkt);
            } catch (Throwable t) {
                Log.w(TAG, "g20 pipe", t);
            }
            return;
        }
        if (udpPipeOk && udpPipe != null) {
            try {
                udpPipe.writeData(pkt);
            } catch (Throwable t) {
                Log.w(TAG, "udp pipe", t);
            }
            return;
        }
        if (udp != null && robotAddr != null) {
            try {
                udp.send(new DatagramPacket(pkt, pkt.length, robotAddr, ROBOT_PORT));
            } catch (Exception e) {
                Log.w(TAG, "udp send", e);
            }
        }
    }

    private synchronized void openPipes() {
        closePipes();
        try {
            g20Pipe = PipelineManager.INSTANCE.createG12G20Pipeline();
            if (g20Pipe != null) {
                g20Pipe.setOnCommListener(pipeListener("g20", true));
                PipelineManager.INSTANCE.connectPipeline(g20Pipe);
            }
        } catch (Throwable t) {
            Log.w(TAG, "g20 pipe", t);
        }
        try {
            udpPipe = PipelineManager.INSTANCE.createUDPPipeline(
                    ROBOT_PORT, AIR_IP, ROBOT_PORT);
            if (udpPipe != null) {
                udpPipe.setOnCommListener(pipeListener("udp144", false));
                PipelineManager.INSTANCE.connectPipeline(udpPipe);
            }
        } catch (Throwable t) {
            Log.w(TAG, "udp pipe", t);
        }
    }

    private CommListener pipeListener(final String tag, final boolean g20) {
        return new CommListener() {
            @Override
            public void onConnectSuccess() {
                if (g20) g20Ok = true;
                else udpPipeOk = true;
                Log.i(TAG, tag + " up");
            }

            @Override
            public void onConnectFail(@Nullable SkyException e) {
                if (g20) g20Ok = false;
                else udpPipeOk = false;
                Log.w(TAG, tag + " fail " + e);
            }

            @Override
            public void onDisconnect() {
                if (g20) g20Ok = false;
                else udpPipeOk = false;
            }

            @Override
            public void onReadData(@Nullable byte[] data) {
            }
        };
    }

    private void closePipes() {
        g20Ok = false;
        udpPipeOk = false;
        if (g20Pipe != null) {
            try {
                PipelineManager.INSTANCE.disconnectPipeline(g20Pipe);
            } catch (Throwable ignored) {
            }
            g20Pipe = null;
        }
        if (udpPipe != null) {
            try {
                PipelineManager.INSTANCE.disconnectPipeline(udpPipe);
            } catch (Throwable ignored) {
            }
            udpPipe = null;
        }
    }
}
