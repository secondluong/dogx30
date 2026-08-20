package com.dogx30.control;

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
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * 2.4G 备份：把起立/趴下/行走等 0x21 指令直发运动主机，不经网关。
 * UDP 走 192.168.1.103:43893；同时写入 G20 数传管道，网关 WiFi 不通时仍能到狗。
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
    private boolean emergency;
    private boolean prevStand;
    private boolean prevSit;
    private boolean prevEstop;
    private int tick;
    @Nullable private DatagramSocket udp;
    @Nullable private InetAddress robotAddr;
    @Nullable private Pipeline pipe;

    private final Runnable loop = this::onTick;

    synchronized void setEnabled(boolean on) {
        if (enabled == on) return;
        enabled = on;
        if (on) start();
        else stop();
    }

    synchronized void command(String name) {
        if (!enabled || name == null) return;
        switch (name) {
            case "stand_up":
                sendSimple(emergency ? UNLOAD : STAND);
                emergency = false;
                standing = true;
                break;
            case "sit":
            case "sit_down":
                sendSimple(SIT);
                standing = false;
                break;
            case "stand":
                if (emergency) {
                    sendSimple(UNLOAD);
                    emergency = false;
                    standing = false;
                } else if (standing) {
                    sendSimple(SIT);
                    standing = false;
                } else {
                    sendSimple(STAND);
                    standing = true;
                }
                break;
            case "unload":
                sendSimple(UNLOAD);
                emergency = false;
                standing = false;
                break;
            case "torque":
                sendSimple(TORQUE);
                break;
            case "step":
                sendSimple(STEP);
                break;
            case "estop":
                sendSimple(ESTOP);
                emergency = true;
                standing = false;
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
        try {
            robotAddr = InetAddress.getByName(ROBOT_IP);
            udp = new DatagramSocket();
        } catch (Exception e) {
            Log.w(TAG, "udp", e);
        }
        try {
            pipe = PipelineManager.INSTANCE.createG12G20Pipeline();
            if (pipe != null) {
                pipe.setOnCommListener(new CommListener() {
                    @Override public void onConnectSuccess() { }
                    @Override public void onConnectFail(SkyException e) {
                        Log.w(TAG, "pipe fail " + e);
                    }
                    @Override public void onDisconnect() { }
                    @Override public void onReadData(byte[] data) { }
                });
                PipelineManager.INSTANCE.connectPipeline(pipe);
            }
        } catch (Throwable t) {
            Log.w(TAG, "pipe", t);
        }
        if (!running) {
            running = true;
            handler.post(loop);
        }
    }

    private void stop() {
        running = false;
        handler.removeCallbacks(loop);
        if (udp != null) {
            udp.close();
            udp = null;
        }
        if (pipe != null) {
            try {
                PipelineManager.INSTANCE.disconnectPipeline(pipe);
            } catch (Throwable t) {
                Log.w(TAG, "pipe close", t);
            }
            pipe = null;
        }
    }

    private void onTick() {
        if (!running || !enabled) return;
        handler.postDelayed(loop, TICK_MS);
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
            sendAxes(snap.ch);
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
        if (udp != null && robotAddr != null) {
            try {
                udp.send(new DatagramPacket(pkt, pkt.length, robotAddr, ROBOT_PORT));
            } catch (Exception e) {
                Log.w(TAG, "udp send", e);
            }
        }
        if (pipe != null) {
            try {
                if (pipe.isConnected()) pipe.writeData(pkt);
            } catch (Throwable t) {
                Log.w(TAG, "pipe send", t);
            }
        }
    }
}
