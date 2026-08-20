package com.dogx30.control;

import android.content.Context;
import android.content.SharedPreferences;

/** App 本机记下的网关地址。和狗上 gateway.conf 里的运动/感知主机不是一回事。 */
final class GatewayStore {
    static final String PREFS = "x30";
    static final String KEY_HOST = "host";
    static final String KEY_PORT = "port";
    static final String KEY_RADIO = "radioPath";
    static final String DEFAULT_HOST = "192.168.1.32";
    static final int DEFAULT_PORT = 8080;

    static SharedPreferences prefs(Context ctx) {
        return ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    static String host(Context ctx) {
        return prefs(ctx).getString(KEY_HOST, DEFAULT_HOST);
    }

    static int port(Context ctx) {
        return prefs(ctx).getInt(KEY_PORT, DEFAULT_PORT);
    }

    static void save(Context ctx, String host, int port) {
        prefs(ctx).edit().putString(KEY_HOST, host).putInt(KEY_PORT, port).apply();
    }

    static String consoleUrl(Context ctx) {
        return "http://" + host(ctx) + ":" + port(ctx) + "/index.html?shell=app";
    }

    static String radioPath(Context ctx) {
        String v = prefs(ctx).getString(KEY_RADIO, "mesh");
        return "radio".equals(v) ? "radio" : "mesh";
    }

    static void saveRadioPath(Context ctx, String path) {
        prefs(ctx).edit()
                .putString(KEY_RADIO, "radio".equals(path) ? "radio" : "mesh")
                .apply();
    }

    private GatewayStore() {}
}
