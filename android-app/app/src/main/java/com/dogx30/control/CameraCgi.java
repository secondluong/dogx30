package com.dogx30.control;

import android.net.Network;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.SecureRandom;
import java.security.cert.X509Certificate;
import java.util.ArrayDeque;
import java.util.Iterator;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLContext;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

/**
 * 布控球 anv CGI。球在 10 网，socket 钉 WiFi，不走 2.4G 图传口。
 * 球机证书自签，这里只信这一次 HTTP(S) 调用，不改系统信任库。
 */
final class CameraCgi {
    private static final String TAG = "CameraCgi";
    private static final int CONNECT_MS = 400;
    private static final int READ_MS = 800;
    private static final int FIRE_CONNECT_MS = 250;
    private static final int FIRE_READ_MS = 200;
    private static final ExecutorService IO = Executors.newSingleThreadExecutor();
    private static final Object FIRE_LOCK = new Object();
    private static final ArrayDeque<String> fireQueue = new ArrayDeque<>();
    private static boolean fireBind;
    private static boolean fireBusy;
    private static volatile HttpURLConnection fireConn;

    private CameraCgi() {}

    /**
     * 只保留最新一条。转动还堵在读响应时，停转会拆掉那条连接马上发出去，
     * 否则松杆后球还要再转一整段超时。
     */
    static void fire(String url, boolean bindRadio) {
        if (url == null || url.isEmpty()) return;
        final boolean stop = url.contains("action=Stop");
        synchronized (FIRE_LOCK) {
            fireBind = bindRadio;
            if (stop) {
                fireQueue.clear();
                fireQueue.add(url);
                HttpURLConnection pending = fireConn;
                if (pending != null) {
                    try {
                        pending.disconnect();
                    } catch (Exception ignored) {
                    }
                }
            } else {
                if (!fireQueue.isEmpty() && fireQueue.peekLast().contains("action=Stop")) {
                    fireQueue.clear();
                }
                String kind = actionKind(url);
                Iterator<String> it = fireQueue.iterator();
                while (it.hasNext()) {
                    if (kind.equals(actionKind(it.next()))) it.remove();
                }
                fireQueue.add(url);
            }
            if (fireBusy) return;
            fireBusy = true;
        }
        IO.execute(CameraCgi::drainFire);
    }

    private static String actionKind(String url) {
        if (url.contains("ZoomAdd") || url.contains("ZoomSub")) return "zoom";
        if (url.contains("action=Stop")) return "stop";
        return "move";
    }

    private static void drainFire() {
        while (true) {
            String url;
            boolean bindRadio;
            synchronized (FIRE_LOCK) {
                if (fireQueue.isEmpty()) {
                    fireBusy = false;
                    return;
                }
                url = fireQueue.removeFirst();
                bindRadio = fireBind;
            }
            try {
                fetch(url, bindRadio, FIRE_CONNECT_MS, FIRE_READ_MS, true);
            } catch (Exception e) {
                Log.w(TAG, brief(e));
            }
        }
    }

    private static final String USER_MD5 = "21232f297a57a5a743894a0e4a801fc3";
    private static final String PWD_MD5 = "21232f297a57a5a743894a0e4a801fc3";

    static int getPipMode(String host) {
        String h = host == null ? "" : host.trim();
        if (h.isEmpty()) h = "192.168.10.168";
        String body = get("http://" + h + "/cgi-bin/anv/pip_cgi?user=" + USER_MD5
                + "&pwd=" + PWD_MD5 + "&action=get&Cache=" + Math.random(), false);
        if (body == null) return -1;
        for (String line : body.split("\\r?\\n")) {
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            if (!"mode".equalsIgnoreCase(line.substring(0, eq).trim())) continue;
            try {
                int m = Integer.parseInt(line.substring(eq + 1).trim());
                if (m >= 0 && m <= 5) return m;
            } catch (NumberFormatException e) {
                return -1;
            }
        }
        return -1;
    }

    static void setPip(String host, int mode) {
        fire(pipSetUrl(host, mode), false);
    }

    static void setPipNow(String host, int mode) {
        get(pipSetUrl(host, mode), false);
    }

    private static String pipSetUrl(String host, int mode) {
        String h = host == null ? "" : host.trim();
        if (h.isEmpty()) h = "192.168.10.168";
        int m = ((mode % 6) + 6) % 6;
        return "http://" + h + "/cgi-bin/anv/pip_cgi?user=" + USER_MD5
                + "&pwd=" + PWD_MD5
                + "&action=set&mode=" + m
                + "&SmallPicSize=0&SmallPicPos=0&CustomSmallPicX=0&CustomSmallPicY=0"
                + "&Cache=" + Math.random();
    }

    /** 只拉音量。编码必须带着写：球机网页保存也总带 AudioFormat，
     * 漏写会被固件存成 0，喇叭和对讲一起停。 */
    static void boostAudio(String host) {
        String h = host == null ? "" : host.trim();
        if (h.isEmpty()) h = "192.168.10.168";
        final String dest = h;
        IO.execute(() -> {
            String cur = get("http://" + dest + "/cgi-bin/anv/audio_cgi2?user=" + USER_MD5
                    + "&pwd=" + PWD_MD5 + "&action=get&Cache=" + Math.random(), false);
            int fmt = parseAudioFormat(cur);
            // 0 不是合法编码。网页只认 19=G.711A、37=AAC。
            // 19 会把 RTSP 也切成 PCMA，ExoPlayer 放不出来，恢复用 AAC。
            if (fmt != 19 && fmt != 37) fmt = 37;
            get("http://" + dest + "/cgi-bin/anv/audio_cgi2?user=" + USER_MD5
                    + "&pwd=" + PWD_MD5
                    + "&action=set&IODevice=0&InputVolume=100&OutputVolume=100"
                    + "&EnableAudio=1&AudioFormat=" + fmt
                    + "&Cache=" + Math.random(), false);
        });
    }

    private static int parseAudioFormat(String body) {
        if (body == null) return 0;
        for (String line : body.split("\\r?\\n")) {
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            if (!"AudioFormat".equalsIgnoreCase(line.substring(0, eq).trim())) continue;
            try {
                return Integer.parseInt(line.substring(eq + 1).trim());
            } catch (NumberFormatException e) {
                return 0;
            }
        }
        return 0;
    }

    static String get(String url, boolean bindRadio) {
        if (url == null || url.isEmpty()) return "";
        try {
            return fetch(url, bindRadio, CONNECT_MS, READ_MS, false);
        } catch (Exception e) {
            Log.w(TAG, brief(e));
            return "";
        }
    }

    private static String fetch(String spec, boolean bindRadio, int connectMs, int readMs,
                                boolean trackFire) throws Exception {
        URL url = new URL(spec);
        HttpURLConnection conn;
        Network net = bindRadio
                ? RadioLink.get().airNetwork()
                : RadioLink.get().meshNetwork();
        if (net != null) {
            conn = (HttpURLConnection) net.openConnection(url);
        } else {
            conn = (HttpURLConnection) url.openConnection();
        }
        if (conn instanceof HttpsURLConnection) {
            trust((HttpsURLConnection) conn);
        }
        conn.setConnectTimeout(connectMs);
        conn.setReadTimeout(readMs);
        conn.setUseCaches(false);
        conn.setRequestMethod("GET");
        conn.setInstanceFollowRedirects(true);
        if (trackFire) fireConn = conn;
        try (InputStream in = conn.getInputStream()) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[256];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            return out.toString("UTF-8");
        } finally {
            if (trackFire && fireConn == conn) fireConn = null;
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
