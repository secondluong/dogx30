package com.dogx30.control;

import android.net.Network;
import android.os.Handler;
import android.os.Looper;
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

/**
 * App → 网关的 WebSocket。socket 钉在 WiFi / MESH 电台上，避开 2.4G 的 ar_net0。
 * 下行不走 evaluateJavascript 拼 JSON：10 Hz 遥测把整段塞进 JS 源会卡住 WebView，
 * 连接看着就像 1–2 秒断一次。消息进队列，由网页 wsPoll 取。
 */
final class NativeWs {
    private static final String TAG = "NativeWs";

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
