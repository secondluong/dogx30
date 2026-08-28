package com.dogx30.control;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import com.skydroid.rcsdk.KeyManager;
import com.skydroid.rcsdk.RCSDKManager;
import com.skydroid.rcsdk.SDKManagerCallBack;
import com.skydroid.rcsdk.common.callback.CompletionCallback;
import com.skydroid.rcsdk.common.callback.CompletionCallbackWith;
import com.skydroid.rcsdk.common.error.SkyException;
import com.skydroid.rcsdk.key.AirLinkKey;
import com.skydroid.rcsdk.key.RemoteControllerKey;
import com.skydroid.rcsdk.utils.RCSDKUtils;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * 云卓 G20 通道读取。映射写死在网页里，这里只轮询 PWM 数组。
 *
 * G20 必须 {@code get(KeyChannels)}，间隔 ≥100ms，没有推送。
 */
public final class G20Rc {

    public interface Listener {
        void onRcUpdate(@NonNull Snapshot snap);
    }

    public static final class Snapshot {
        public final boolean connected;
        public final String device;
        public final String error;
        public final int[] ch;
        public final int seq;

        Snapshot(boolean connected, String device, String error, int[] ch, int seq) {
            this.connected = connected;
            this.device = device == null ? "" : device;
            this.error = error == null ? "" : error;
            this.ch = ch;
            this.seq = seq;
        }

        @NonNull
        public String toJson() {
            try {
                JSONObject o = new JSONObject();
                o.put("seq", seq);
                o.put("connected", connected);
                o.put("device", device);
                o.put("error", error);
                JSONArray arr = new JSONArray();
                for (int v : ch) arr.put(v);
                o.put("ch", arr);
                return o.toString();
            } catch (Exception e) {
                return "{}";
            }
        }
    }

    private static final String TAG = "G20Rc";
    private static final long POLL_MS = 100;
    private static final G20Rc INST = new G20Rc();

    public static G20Rc get() {
        return INST;
    }

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final List<Listener> listeners = new CopyOnWriteArrayList<>();
    private boolean started;
    private boolean connected;
    private boolean polling;
    private boolean backupRadio;
    private boolean radioActivatedThisSession;
    private String device = "";
    private String error = "";
    private int[] ch = new int[0];
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
            // USB 网这时才稳，跟云深处一样再开到运动主机的 UDP。
            RadioLink.get().onRcReady();
        }

        @Override
        public void onRcConnectFail(@Nullable SkyException e) {
            connected = false;
            error = e == null
                    ? "遥控器连接失败。请先关掉云卓助手和其他地面站。"
                    : "遥控器连接失败：" + e;
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
        started = true;
        RadioLink.get().attach(context);
        try {
            RCSDKManager.INSTANCE.initSDK(context.getApplicationContext(), sdkCb);
            RCSDKManager.INSTANCE.setMainThreadCallBack(true);
            RCSDKManager.INSTANCE.connectToRC();
            refreshDevice();
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
        return new Snapshot(connected, device, error, Arrays.copyOf(ch, ch.length), seq);
    }

    public synchronized String pollJson() {
        return snapshot().toJson();
    }

    /** 只有顶栏明确切到 2.4G 才开直达。开机/断线不要动射频，否则 G20 画面会没。 */
    public void setBackupRadio(boolean on) {
        backupRadio = on;
        RadioLink.get().setEnabled(on);
        // 用过 2.4G 后切 MESH 只停运动心跳，不拆射频网卡。拆掉再立即重建时
        // ar_net0/SDK UDP 管道经常回不来，表现为再切回 2.4G 后所有按钮失效。
        // MESH 期间进程出口由 RadioLink 钉在 WiFi，保留射频网卡不会抢控制权。
        if (on) {
            radioActivatedThisSession = true;
            enableRf(true);
        } else if (!radioActivatedThisSession) {
            // App 以 MESH 启动时要清理上个进程可能遗留的射频状态。
            enableRf(false);
        }
        if (on && connected) RadioLink.get().onRcReady();
    }

    private void enableRf(boolean on) {
        try {
            KeyManager.INSTANCE.set(
                    AirLinkKey.INSTANCE.getKeyRCRFEnable(),
                    Boolean.valueOf(on),
                    new CompletionCallback() {
                        @Override
                        public void onResult(@Nullable SkyException e) {
                            Log.i(TAG, "RF " + on + " " + e);
                        }
                    });
        } catch (Throwable t) {
            Log.w(TAG, "RF", t);
        }
    }

    public void setWsDown(boolean down) {
        // 网关断开不再自动开数传。上一版会把画面和控制一起掐死。
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
        ch = Arrays.copyOf(value, value.length);
        seq++;
        emit();
    }

    private void refreshDevice() {
        try {
            Object t = RCSDKUtils.getDeviceType();
            device = t != null ? String.valueOf(t)
                    : String.valueOf(RCSDKManager.INSTANCE.getDeviceType());
        } catch (Throwable t) {
            device = "UNKNOWN";
        }
    }

    private void emit() {
        Snapshot snap = snapshot();
        for (Listener l : listeners) l.onRcUpdate(snap);
    }
}
