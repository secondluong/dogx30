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
 * 仅在操作员明确切到 2.4G 时，把 0x21 直发运动主机。
 * MESH/WiFi 到不了 .103。指令走 G20 {@code createUDPPipeline}；
 * 本机 UDP 只绑 USB/网口，绝不回退到网关 WiFi。
 * 不开关射频，也不开串口数传管道，那两条会把画面掐掉。
 */
final class RadioLink {

    private static final String TAG = "RadioLink";
    private static final String ROBOT_IP = "192.168.1.103";
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
    private boolean pipeOk;
    private boolean boundUsb;
    @Nullable private Context appCtx;
    @Nullable private DatagramSocket udp;
    @Nullable private InetAddress robotAddr;
    @Nullable private Pipeline pipe;

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
        return enabled && (pipeOk || boundUsb);
    }

    // 旧网页曾用虚拟摇杆灌轴。起立后发轴会把狗锁进踏步，这里直接丢掉。
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
        openUdp();
        openSdkUdp();
        if (!running) {
            running = true;
            handler.post(loop);
        }
    }

    private synchronized void openUdp() {
        try {
            robotAddr = InetAddress.getByName(ROBOT_IP);
            if (udp != null) {
                udp.close();
                udp = null;
            }
            udp = new DatagramSocket();
            bindToAirlink(udp);
        } catch (Exception e) {
            Log.w(TAG, "udp", e);
        }
    }

    private void bindToAirlink(DatagramSocket sock) {
        boundUsb = false;
        if (appCtx == null) return;
        ConnectivityManager cm = (ConnectivityManager)
                appCtx.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm == null) return;
        Network pick = null;
        for (Network n : cm.getAllNetworks()) {
            NetworkCapabilities caps = cm.getNetworkCapabilities(n);
            LinkProperties lp = cm.getLinkProperties(n);
            if (lp == null || !hasLanV4(lp)) continue;
            if (isWired(caps)) {
                pick = n;
                break;
            }
        }
        if (pick == null) return;
        try {
            pick.bindSocket(sock);
            boundUsb = true;
            Log.i(TAG, "udp bound usb " + pick);
        } catch (Exception e) {
            Log.w(TAG, "bindSocket", e);
        }
    }

    private synchronized void openSdkUdp() {
        closeSdkUdp();
        try {
            pipe = PipelineManager.INSTANCE.createUDPPipeline(ROBOT_IP, ROBOT_PORT);
            if (pipe == null) {
                Log.w(TAG, "createUDPPipeline null");
                return;
            }
            pipe.setOnCommListener(new CommListener() {
                @Override
                public void onConnectSuccess() {
                    pipeOk = true;
                    Log.i(TAG, "udp pipeline up");
                }

                @Override
                public void onConnectFail(@Nullable SkyException e) {
                    pipeOk = false;
                    Log.w(TAG, "udp pipeline fail " + e);
                }

                @Override
                public void onDisconnect() {
                    pipeOk = false;
                }

                @Override
                public void onReadData(@Nullable byte[] data) {
                }
            });
            PipelineManager.INSTANCE.connectPipeline(pipe);
        } catch (Throwable t) {
            Log.w(TAG, "openSdkUdp", t);
        }
    }

    private void closeSdkUdp() {
        pipeOk = false;
        if (pipe == null) return;
        try {
            PipelineManager.INSTANCE.disconnectPipeline(pipe);
        } catch (Throwable ignored) {
        }
        pipe = null;
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

    private void stop() {
        running = false;
        handler.removeCallbacks(loop);
        standing = false;
        clearWalk();
        confirmed = false;
        closeSdkUdp();
        boundUsb = false;
        if (udp != null) {
            udp.close();
            udp = null;
        }
    }

    private void onTick() {
        if (!running || !enabled) return;
        handler.postDelayed(loop, TICK_MS);
        G20Rc.Snapshot snap = G20Rc.get().snapshot();
        if (tick % 50 == 0 && !pipeOk) {
            openSdkUdp();
        }
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
        // 轴只能在力控/踏步里发。起立后就灌零轴，主机会当成压身高，再点
        // 力控/起步就会原地踏步，坐下和步态也被冲掉。
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
        // 只走一条路。两条一起发时，起步这种切换指令会被打两次抵消。
        if (pipe != null && pipeOk) {
            try {
                pipe.writeData(pkt);
            } catch (Throwable t) {
                Log.w(TAG, "pipe send", t);
            }
            return;
        }
        if (boundUsb && udp != null && robotAddr != null) {
            try {
                udp.send(new DatagramPacket(pkt, pkt.length, robotAddr, ROBOT_PORT));
            } catch (Exception e) {
                Log.w(TAG, "udp send", e);
            }
        }
    }
}
