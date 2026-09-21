package com.dogx30.control;

import android.net.Network;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.SecureRandom;
import java.security.cert.X509Certificate;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLContext;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

/**
 * 布控球 anv CGI。2.4G 时绑射频网卡，和 NativeVideo 拉 RTSP 同一条路。
 * 球机证书自签，这里只信这一次 HTTP(S) 调用，不改系统信任库。
 */
final class CameraCgi {
    private static final String TAG = "CameraCgi";
    private static final int CONNECT_MS = 400;
    private static final int READ_MS = 800;
    private static final ExecutorService IO = Executors.newSingleThreadExecutor();

    private CameraCgi() {}

    static void fire(String url, boolean bindRadio) {
        if (url == null || url.isEmpty()) return;
        IO.execute(() -> get(url, bindRadio));
    }

    static String get(String url, boolean bindRadio) {
        if (url == null || url.isEmpty()) return "";
        try {
            return fetch(url, bindRadio);
        } catch (Exception e) {
            Log.w(TAG, brief(e));
            return "";
        }
    }

    private static String fetch(String spec, boolean bindRadio) throws Exception {
        URL url = new URL(spec);
        HttpURLConnection conn;
        Network net = bindRadio ? RadioLink.get().airNetwork() : null;
        if (net != null) {
            conn = (HttpURLConnection) net.openConnection(url);
        } else {
            conn = (HttpURLConnection) url.openConnection();
        }
        if (conn instanceof HttpsURLConnection) {
            trust((HttpsURLConnection) conn);
        }
        conn.setConnectTimeout(CONNECT_MS);
        conn.setReadTimeout(READ_MS);
        conn.setUseCaches(false);
        conn.setRequestMethod("GET");
        conn.setInstanceFollowRedirects(true);
        try (InputStream in = conn.getInputStream()) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[256];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            return out.toString("UTF-8");
        } finally {
            conn.disconnect();
        }
    }

    private static void trust(HttpsURLConnection conn) throws Exception {
        TrustManager[] tm = new TrustManager[]{
                new X509TrustManager() {
                    @Override
                    public void checkClientTrusted(X509Certificate[] c, String a) {}

                    @Override
                    public void checkServerTrusted(X509Certificate[] c, String a) {}

                    @Override
                    public X509Certificate[] getAcceptedIssuers() {
                        return new X509Certificate[0];
                    }
                }
        };
        SSLContext ctx = SSLContext.getInstance("TLS");
        ctx.init(null, tm, new SecureRandom());
        conn.setSSLSocketFactory(ctx.getSocketFactory());
        conn.setHostnameVerifier((host, session) -> true);
    }

    private static String brief(Throwable e) {
        String m = e.getMessage();
        if (m == null || m.isEmpty()) return e.getClass().getSimpleName();
        return m;
    }
}
