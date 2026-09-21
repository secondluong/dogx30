package com.dogx30.control;

import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.webkit.WebView;

import androidx.annotation.Nullable;

import org.json.JSONObject;

import java.util.concurrent.TimeUnit;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;

/**
 * App → 网关的 WebSocket。进程出口由 RadioLink.pinProcess 钉在 10 网。
 * 不用 Network.getSocketFactory：选错 Network 时 10.2 会彻底连不上（MESH 一直黄）。
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
            Log.i(TAG, "open " + want);
            try {
                Request req = new Request.Builder().url(want).build();
                socket = client.newWebSocket(req, new WebSocketListener() {
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
                        if (my != gen) return;
                        js("if(window.X30NativeWs)X30NativeWs.handleWsText("
                                + JSONObject.quote(text) + ")");
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
