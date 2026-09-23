package com.dogx30.control;

import android.net.Network;
import android.os.Handler;
import android.os.Looper;
import android.util.Base64;
import android.util.Log;
import android.webkit.WebView;

import androidx.annotation.Nullable;

import org.json.JSONArray;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;
import okio.ByteString;

/**
 * App → 网关的 WebSocket。socket 走 WiFi / MESH 电台。
 * 下行不走 evaluateJavascript 拼 JSON：10 Hz 遥测把整段塞进 JS 源会卡住 WebView，
 * 连接看着就像 1–2 秒断一次。消息进队列，由网页 wsPoll / wsPollBin 取。
 *
 * 点云是二进制帧。只接 String 时 App 壳永远看不到点，表现就是「已订阅 / 0 点」。
 */
final class NativeWs {
    private static final String TAG = "NativeWs";
    /** 点云只要最新几帧；积压只会把延迟越滚越大。 */
    private static final int BIN_MAX = 3;

    private final WebView web;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final OkHttpClient client = new OkHttpClient.Builder()
            .connectTimeout(4, TimeUnit.SECONDS)
            .readTimeout(0, TimeUnit.SECONDS)
            .writeTimeout(8, TimeUnit.SECONDS)
            .pingInterval(8, TimeUnit.SECONDS)
            .retryOnConnectionFailure(false)
            .build();

    @Nullable private volatile WebSocket socket;
    private volatile boolean live;
    private static volatile boolean anyLive;
    private int gen;
    private String url = "";
    private final List<String> inbox = new ArrayList<>();
    private final List<byte[]> binInbox = new ArrayList<>();

    NativeWs(WebView web) {
        this.web = web;
    }

    void open(String next) {
        ui.post(() -> {
            final String want = next == null ? "" : next.trim();
            if (want.isEmpty()) {
                js("if(window.X30NativeWs)X30NativeWs.onWsClose()");
                return;
            }
            if (want.equals(url) && socket != null && live) {
                js("if(window.X30NativeWs)X30NativeWs.onWsOpen()");
                return;
            }
            final int my = ++gen;
            live = false;
            anyLive = false;
            if (socket != null) {
                try {
                    socket.cancel();
                } catch (Exception ignored) {
                }
                socket = null;
            }
            url = want;
            synchronized (inbox) { inbox.clear(); }
            synchronized (binInbox) { binInbox.clear(); }
            Log.i(TAG, "open " + want);
            try {
                OkHttpClient c = client;
                Network mesh = RadioLink.get().meshNetwork();
                if (mesh != null) {
                    c = client.newBuilder().socketFactory(mesh.getSocketFactory()).build();
                }
                Request req = new Request.Builder().url(want).build();
                socket = c.newWebSocket(req, new WebSocketListener() {
                    @Override
                    public void onOpen(WebSocket ws, Response response) {
                        if (my != gen) return;
                        live = true;
                        anyLive = true;
                        Log.i(TAG, "up " + want);
                        js("if(window.X30NativeWs)X30NativeWs.onWsOpen()");
                    }

                    @Override
                    public void onMessage(WebSocket ws, String text) {
                        if (my != gen || text == null) return;
                        synchronized (inbox) {
                            if (inbox.size() > 80) inbox.remove(0);
                            inbox.add(text);
                        }
                    }

                    @Override
                    public void onMessage(WebSocket ws, ByteString bytes) {
                        if (my != gen || bytes == null || bytes.size() == 0) return;
                        synchronized (binInbox) {
                            if (binInbox.size() >= BIN_MAX) binInbox.remove(0);
                            binInbox.add(bytes.toByteArray());
                        }
                    }

                    @Override
                    public void onClosed(WebSocket ws, int code, String reason) {
                        if (dead(my)) js("if(window.X30NativeWs)X30NativeWs.onWsClose()");
                    }

                    @Override
                    public void onFailure(WebSocket ws, Throwable t, Response response) {
                        if (!dead(my)) return;
                        Log.w(TAG, "fail " + url, t);
                        js("if(window.X30NativeWs)X30NativeWs.onWsClose()");
                    }
                });
            } catch (Exception e) {
                if (dead(my)) {
                    Log.w(TAG, "open", e);
                    js("if(window.X30NativeWs)X30NativeWs.onWsClose()");
                }
            }
        });
    }

    void send(String text) {
        WebSocket ws = socket;
        if (ws == null || text == null) return;
        ws.send(text);
    }

    void close() {
        ui.post(() -> {
            gen++;
            dead(gen);
            url = "";
        });
    }

    boolean isLive() {
        return live && socket != null;
    }

    String poll() {
        synchronized (inbox) {
            if (inbox.isEmpty()) return "[]";
            JSONArray a = new JSONArray();
            for (String s : inbox) a.put(s);
            inbox.clear();
            return a.toString();
        }
    }

    /** 点云二进制帧，Base64 数组。网页还原成 ArrayBuffer 再交给 cloud.js。 */
    String pollBin() {
        synchronized (binInbox) {
            if (binInbox.isEmpty()) return "[]";
            JSONArray a = new JSONArray();
            for (byte[] b : binInbox) {
                a.put(Base64.encodeToString(b, Base64.NO_WRAP));
            }
            binInbox.clear();
            return a.toString();
        }
    }

    static boolean isAnyLive() {
        return anyLive;
    }

    private boolean dead(int my) {
        if (my != gen) return false;
        if (!live && socket == null) return false;
        live = false;
        anyLive = false;
        WebSocket ws = socket;
        socket = null;
        if (ws != null) {
            try {
                ws.cancel();
            } catch (Exception ignored) {
            }
        }
        return true;
    }

    private void js(String code) {
        ui.post(() -> {
            if (web != null) web.evaluateJavascript(code, null);
        });
    }
}
