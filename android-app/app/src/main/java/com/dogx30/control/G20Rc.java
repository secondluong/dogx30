package com.dogx30.control;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import com.skydroid.rcsdk.KeyManager;
import com.skydroid.rcsdk.RCSDKManager;
import com.skydroid.rcsdk.SDKManagerCallBack;
import com.skydroid.rcsdk.common.callback.CompletionCallbackWith;
import com.skydroid.rcsdk.common.error.SkyException;
import com.skydroid.rcsdk.key.RemoteControllerKey;
import com.skydroid.rcsdk.utils.RCSDKUtils;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * 云卓 G20 通道读取。
 *
 * Android KeyEvent / Gamepad API 看不到这台手柄的 L1/L2/摇杆。
 * G20 必须轮询 {@code RemoteControllerKey.KeyChannels}，间隔 ≥100ms，没有推送。
 * 返回值是 PWM 通道（大约 1000–2000），不是 keyCode。
 *
 * 出厂通道表可在设备助手里改，所以名字不写死：点名称再按对应键完成绑定。
 * 这里只读通道，不向机器狗发运动指令。
 */
public final class G20Rc {

    public static final String[] NAMES = {
            "L1", "L2", "R1", "R2", "B1", "B2", "PHOTO", "WHEEL", "H", "PAUSE", "MODE"
    };

    public interface Listener {
        void onRcUpdate(@NonNull Snapshot snap);
    }

    public static final class Snapshot {
        public final boolean started;
        public final boolean connected;
        public final String device;
        public final String error;
        public final int[] ch;
        public final int lastCh;
        public final String lastName;
        public final String[] down;
        public final Map<String, Integer> binds;
        public final String armed;
        public final int seq;

        Snapshot(boolean started, boolean connected, String device, String error,
                 int[] ch, int lastCh, String lastName, String[] down,
                 Map<String, Integer> binds, String armed, int seq) {
            this.started = started;
            this.connected = connected;
            this.device = device;
            this.error = error == null ? "" : error;
            this.ch = ch;
            this.lastCh = lastCh;
            this.lastName = lastName == null ? "" : lastName;
            this.down = down;
            this.binds = binds;
            this.armed = armed == null ? "" : armed;
            this.seq = seq;
        }

        @NonNull
        public String toJson() {
            try {
                JSONObject o = new JSONObject();
                o.put("seq", seq);
                o.put("started", started);
                o.put("connected", connected);
                o.put("device", device);
                o.put("error", error);
                o.put("last", lastCh);
                o.put("lastName", lastName);
                o.put("armed", armed);
                JSONArray arr = new JSONArray();
                for (int v : ch) arr.put(v);
                o.put("ch", arr);
                JSONArray d = new JSONArray();
                for (String n : down) d.put(n);
                o.put("down", d);
                JSONObject b = new JSONObject();
                for (Map.Entry<String, Integer> e : binds.entrySet()) {
                    b.put(e.getKey(), e.getValue());
                }
                o.put("binds", b);
                return o.toString();
            } catch (Exception e) {
                return "{}";
            }
        }
    }

    private static final String TAG = "G20Rc";
    private static final String PREFS = "x30_g20";
    private static final String KEY_BINDS = "binds";
    private static final long POLL_MS = 100;
    private static final int MOVE_EPS = 80;
    private static final int BIND_EPS = 180;
    private static final int STICK_CH = 4;
    private static final int DOWN_EPS = 250;

    private static final G20Rc INST = new G20Rc();

    public static G20Rc get() {
        return INST;
    }

    public static String labelOf(String name) {
        if ("PHOTO".equals(name)) return "拍照";
        if ("WHEEL".equals(name)) return "滚轮";
        if ("PAUSE".equals(name)) return "暂停";
        if ("MODE".equals(name)) return "三段";
        return name;
    }

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final List<Listener> listeners = new CopyOnWriteArrayList<>();
    private final Map<String, Integer> binds = new LinkedHashMap<>();
    private SharedPreferences prefs;
    private boolean started;
    private boolean connected;
    private boolean polling;
    private String device = "—";
    private String error = "正在连接云卓 RCSDK…";
    private String armed = "";
    private int[] ch = new int[0];
    private int[] prev;
    private int[] rest;
    private int lastCh = -1;
    private String lastName = "";
    private int seq;

    private final Runnable poll = this::requestChannels;

    private final SDKManagerCallBack sdkCb = new SDKManagerCallBack() {
        @Override
        public void onRcConnected() {
            connected = true;
            error = "";
            refreshDevice();
            emit();
            ensurePolling();
        }

        @Override
        public void onRcConnectFail(@Nullable SkyException e) {
            connected = false;
            error = e == null
                    ? "遥控器连接失败。请先关掉云卓助手和其他地面站。"
                    : "遥控器连接失败：" + e + "。请先关掉云卓助手和其他地面站。";
            emit();
        }

        @Override
        public void onRcDisconnect() {
            connected = false;
            if (error.isEmpty()) error = "遥控器已断开";
            emit();
        }
    };

    private final CompletionCallbackWith<int[]> chCb = new CompletionCallbackWith<int[]>() {
        @Override
        public void onSuccess(int[] value) {
            onChannels(value);
        }

        @Override
        public void onFailure(SkyException e) {
            if (e != null && connected) {
                error = "读通道失败：" + e;
                emit();
            }
        }
    };

    public synchronized void start(Context context) {
        if (started) return;
        prefs = context.getApplicationContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        loadBinds();
        started = true;
        try {
            RCSDKManager.INSTANCE.initSDK(context.getApplicationContext(), sdkCb);
            RCSDKManager.INSTANCE.setMainThreadCallBack(true);
            RCSDKManager.INSTANCE.connectToRC();
            refreshDevice();
            error = "已请求连接遥控器。若一直停在这里，先关掉云卓助手。";
        } catch (Throwable t) {
            error = "RCSDK 初始化失败：" + t.getClass().getSimpleName();
            Log.e(TAG, "initSDK", t);
        }
        emit();
        ensurePolling();
    }

    public void addListener(Listener l) {
        if (l == null) return;
        listeners.add(l);
        l.onRcUpdate(snapshot());
    }

    public void removeListener(Listener l) {
        listeners.remove(l);
    }

    public synchronized Snapshot snapshot() {
        return new Snapshot(started, connected, device, error, Arrays.copyOf(ch, ch.length),
                lastCh, lastName, downNow(), new LinkedHashMap<>(binds), armed, seq);
    }

    public synchronized String pollJson() {
        return snapshot().toJson();
    }

    public synchronized void arm(String name) {
        if (!isKnown(name)) return;
        armed = name.equals(armed) ? "" : name;
        emit();
    }

    public synchronized void clearBind(String name) {
        if (binds.remove(name) != null) saveBinds();
        if (name.equals(armed)) armed = "";
        emit();
    }

    private void ensurePolling() {
        if (polling) return;
        polling = true;
        handler.post(poll);
    }

    private void requestChannels() {
        if (!polling) return;
        handler.postDelayed(poll, POLL_MS);
        try {
            KeyManager.INSTANCE.get(RemoteControllerKey.INSTANCE.getKeyChannels(), chCb);
        } catch (Throwable t) {
            Log.w(TAG, "get KeyChannels", t);
        }
    }

    private synchronized void onChannels(int[] value) {
        if (value == null) return;
        if (prev == null || prev.length != value.length) {
            prev = Arrays.copyOf(value, value.length);
            rest = Arrays.copyOf(value, value.length);
            ch = Arrays.copyOf(value, value.length);
            seq++;
            emit();
            return;
        }

        int best = -1;
        int bestDelta = 0;
        int bestNonStick = -1;
        int bestNonStickDelta = 0;
        for (int i = 0; i < value.length; i++) {
            int d = Math.abs(value[i] - prev[i]);
            if (d > bestDelta) {
                bestDelta = d;
                best = i;
            }
            if (i >= STICK_CH && d > bestNonStickDelta) {
                bestNonStickDelta = d;
                bestNonStick = i;
            }
            if (Math.abs(value[i] - rest[i]) < 40) {
                rest[i] = (rest[i] * 3 + value[i]) / 4;
            }
        }

        boolean moved = bestDelta >= MOVE_EPS;
        if (moved) {
            lastCh = best;
            lastName = nameOfChannel(best);
            if (!armed.isEmpty()) {
                tryBind(best, bestDelta, bestNonStick, bestNonStickDelta);
            }
        }

        prev = Arrays.copyOf(value, value.length);
        ch = prev;
        seq++;
        emit();
    }

    private void tryBind(int best, int bestDelta, int nonStick, int nonStickDelta) {
        boolean wheel = "WHEEL".equals(armed);
        int pick;
        if (wheel) {
            pick = nonStickDelta >= BIND_EPS ? nonStick : (bestDelta >= BIND_EPS ? best : -1);
        } else {
            if (nonStickDelta >= BIND_EPS) pick = nonStick;
            else if (best >= STICK_CH && bestDelta >= BIND_EPS) pick = best;
            else {
                error = "刚才动的是摇杆通道 CH" + (best + 1) + "。请按 " + labelOf(armed) + "，不要推杆。";
                return;
            }
        }
        if (pick < 0) return;
        binds.put(armed, pick);
        lastName = armed;
        lastCh = pick;
        error = "已把 " + labelOf(armed) + " 绑到 CH" + (pick + 1) + "。长按名称可清除。";
        armed = "";
        saveBinds();
    }

    private String[] downNow() {
        List<String> out = new ArrayList<>();
        for (Map.Entry<String, Integer> e : binds.entrySet()) {
            int i = e.getValue();
            if (i < 0 || i >= ch.length) continue;
            int restV = (rest != null && i < rest.length) ? rest[i] : 1500;
            if (Math.abs(ch[i] - restV) >= DOWN_EPS) out.add(e.getKey());
        }
        return out.toArray(new String[0]);
    }

    private String nameOfChannel(int idx) {
        for (Map.Entry<String, Integer> e : binds.entrySet()) {
            if (e.getValue() == idx) return e.getKey();
        }
        return "";
    }

    private void refreshDevice() {
        try {
            Object t = RCSDKUtils.getDeviceType();
            if (t != null) device = String.valueOf(t);
            else device = String.valueOf(RCSDKManager.INSTANCE.getDeviceType());
        } catch (Throwable t) {
            device = "UNKNOWN";
        }
    }

    private void loadBinds() {
        binds.clear();
        String raw = prefs.getString(KEY_BINDS, "");
        if (raw == null || raw.isEmpty()) return;
        try {
            JSONObject o = new JSONObject(raw);
            for (String name : NAMES) {
                if (o.has(name)) binds.put(name, o.getInt(name));
            }
        } catch (Exception ignored) {
        }
    }

    private void saveBinds() {
        JSONObject o = new JSONObject();
        try {
            for (Map.Entry<String, Integer> e : binds.entrySet()) {
                o.put(e.getKey(), e.getValue());
            }
            prefs.edit().putString(KEY_BINDS, o.toString()).apply();
        } catch (Exception ignored) {
        }
    }

    private static boolean isKnown(String name) {
        for (String n : NAMES) if (n.equals(name)) return true;
        return false;
    }

    private void emit() {
        Snapshot snap = snapshot();
        for (Listener l : listeners) l.onRcUpdate(snap);
    }
}
