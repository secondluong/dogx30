package com.dogx30.control;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.net.ConnectivityManager;
import android.net.Network;
import android.os.Build;
import android.util.Log;

import androidx.annotation.NonNull;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import org.json.JSONObject;
import org.webrtc.DataChannel;
import org.webrtc.IceCandidate;
import org.webrtc.MediaConstraints;
import org.webrtc.MediaStream;
import org.webrtc.PeerConnection;
import org.webrtc.PeerConnectionFactory;
import org.webrtc.RtpTransceiver;
import org.webrtc.SdpObserver;
import org.webrtc.SessionDescription;
import org.webrtc.SoftwareVideoDecoderFactory;
import org.webrtc.SoftwareVideoEncoderFactory;
import org.webrtc.audio.JavaAudioDeviceModule;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.security.cert.X509Certificate;
import java.util.Arrays;
import java.util.Collections;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

import javax.net.ssl.SSLContext;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

import okhttp3.MediaType;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.RequestBody;
import okhttp3.Response;

/**
 * 布控球对讲。球机网页：先 /rtc/v1/play/ 建「chat」数据通道，收到 interaction
 * 回 OK\\0，再按住把 8 kHz G.711A（每包 320 字节）推上去。
 *
 * App 画面仍走 RTSP。HOME 点一下开麦并能听球机返回声，再点拆掉会话（关麦+静音）。
 * 球机网页在开麦前会先往数据通道发 status=SoundOn，喇叭才接。
 */
final class CameraTalk {
    private static final String TAG = "CameraTalk";
    static final int REQ_MIC = 71;
    private static final int SAMPLE_HZ = 8000;
    private static final int FRAME_SAMPLES = 320; // 40 ms
    private static final int[] REC_RATES = { 48000, 44100, 16000, 8000 };
    private static final CameraTalk INST = new CameraTalk();
    private static final ExecutorService TALK = Executors.newSingleThreadExecutor();

    static CameraTalk get() {
        return INST;
    }

    private final AtomicBoolean on = new AtomicBoolean(false);
    private final AtomicBoolean sessionReady = new AtomicBoolean(false);
    private final AtomicBoolean starting = new AtomicBoolean(false);
    private final AtomicBoolean wantStart = new AtomicBoolean(false);
    private final AtomicInteger talkGen = new AtomicInteger(0);
    private PeerConnectionFactory factory;
    private JavaAudioDeviceModule adm;
    private PeerConnection pc;
    private DataChannel dc;
    private AudioRecord rec;
    private Thread recThread;
    private int recHz = 48000;
    private String pendingHost = "";
    private NativeVideo video;
    private Context appCtx;

    void attachVideo(NativeVideo v) {
        video = v;
    }

    /** 保留给测试；开球不再预建会话，否则状态栏会提前出麦标。 */
    void prepare(Context ctx, String host) {
        String h = host == null ? "" : host.trim();
        if (h.isEmpty()) h = "192.168.10.168";
        pendingHost = h;
    }

    boolean isOn() {
        return on.get() || starting.get();
    }

    /** HOME：已开则全关，没开则开麦+听球。以本机状态为准，不跟网页记忆。 */
    void toggle(Activity act, String host) {
        if (isOn()) {
            stop();
            return;
        }
        start(act, host);
    }

    void start(Activity act, String host) {
        String h = host == null ? "" : host.trim();
        if (h.isEmpty()) h = "192.168.10.168";
        pendingHost = h;
        if (on.get() || starting.get()) return;
        if (ContextCompat.checkSelfPermission(act, Manifest.permission.RECORD_AUDIO)
                != PackageManager.PERMISSION_GRANTED) {
            wantStart.set(true);
            ActivityCompat.requestPermissions(act,
                    new String[]{Manifest.permission.RECORD_AUDIO}, REQ_MIC);
            return;
        }
        wantStart.set(false);
        // 网页桥线程只翻开关。开麦和 WebRTC 丢到后台，否则 WebView 一卡，
        // 画中画 CGI 也跟着死。
        on.set(true);
        NativeVideo v = video;
        if (v != null) v.setTalking(true);
        if (!starting.compareAndSet(false, true)) return;
        CameraCgi.boostAudio(h);
        final String dest = h;
        final Context app = act.getApplicationContext();
        appCtx = app;
        final int gen = talkGen.incrementAndGet();
        TALK.execute(() -> {
            unpin(app);
            try {
                startMic();
            } catch (Exception e) {
                Log.w(TAG, "mic", e);
            }
            runSession(app, dest, true, gen);
        });
    }

    void onPermission(Activity act, int code, int[] grant) {
        if (code != REQ_MIC) return;
        if (grant != null && grant.length > 0
                && grant[0] == PackageManager.PERMISSION_GRANTED
                && wantStart.getAndSet(false)) {
            start(act, pendingHost);
        }
    }

    /** 再点 HOME：关麦，并静音球机返回声。会话拆掉。 */
    void stop() {
        wantStart.set(false);
        starting.set(false);
        talkGen.incrementAndGet();
        on.set(false);
        NativeVideo v = video;
        if (v != null) {
            v.setAdmPlaying(false);
            v.setTalking(false);
        }
        TALK.execute(this::shutdownTalk);
    }

    void release() {
        stop();
    }

    private void shutdownTalk() {
        stopMic();
        if (adm != null) {
            try { adm.setSpeakerMute(true); } catch (Exception ignored) {}
        }
        teardownPeer();
        disposeFactory();
        unpin(appCtx);
    }

    /** 重试时只拆连接，工厂留下。在网页线程里 dispose 工厂会卡死 WebView。 */
    private void teardownSession() {
        teardownPeer();
    }

    private void teardownPeer() {
        sessionReady.set(false);
        DataChannel ch = dc;
        dc = null;
        if (ch != null) {
            try { ch.close(); } catch (Exception ignored) {}
        }
        PeerConnection p = pc;
        pc = null;
        if (p != null) {
            try { p.close(); } catch (Exception ignored) {}
            try { p.dispose(); } catch (Exception ignored) {}
        }
    }

    private void disposeFactory() {
        PeerConnectionFactory f = factory;
        factory = null;
        if (f != null) {
            try { f.dispose(); } catch (Exception ignored) {}
        }
        JavaAudioDeviceModule a = adm;
        adm = null;
        if (a != null) {
            try { a.release(); } catch (Exception ignored) {}
        }
    }

    private void beginTalk() {
        on.set(true);
        sendText("status=SoundOn\0");
        // ADM 喇叭保持静音：WebRTC 音轨经常是空的，再把 RTSP 让开就整段没声。
        // 球机返回声只走 RTSP。ADM 只为建对讲数据通道。
        if (adm != null) {
            try { adm.setSpeakerMute(true); } catch (Exception ignored) {}
        }
        NativeVideo v = video;
        if (v != null) {
            v.setTalking(true);
            v.setAdmPlaying(false);
        }
        ensureMicPump();
    }

    private void ensureMicPump() {
        if (rec == null) {
            try {
                startMic();
            } catch (Exception e) {
                Log.w(TAG, "mic", e);
            }
            return;
        }
        Thread t = recThread;
        if (t == null || !t.isAlive()) {
            recThread = new Thread(this::pumpMic, "ptz-mic");
            recThread.start();
        }
    }

    private void stopMic() {
        AudioRecord r = rec;
        rec = null;
        if (r != null) {
            try { r.stop(); } catch (Exception ignored) {}
            try { r.release(); } catch (Exception ignored) {}
        }
        Thread t = recThread;
        recThread = null;
        if (t != null) t.interrupt();
    }

    private void runSession(Context ctx, String host, boolean startMic, int gen) {
        Context app = ctx.getApplicationContext();
        try {
            for (int attempt = 0; attempt < 3 && on.get() && gen == talkGen.get(); attempt++) {
                if (attempt > 0) {
                    teardownSession();
                    try { Thread.sleep(800); } catch (InterruptedException e) { return; }
                    if (!on.get() || gen != talkGen.get()) return;
                }
                try {
                    connectPlay(app, host, startMic, gen);
                    return;
                } catch (Exception e) {
                    Log.w(TAG, "start " + attempt, e);
                    if (gen == talkGen.get()) teardownSession();
                }
            }
        } finally {
            starting.set(false);
            unpin(app);
        }
    }

    private void connectPlay(Context app, String host, boolean startMic, int gen)
            throws Exception {
        try {
            ensureFactory(app);
            PeerConnection.RTCConfiguration cfg = new PeerConnection.RTCConfiguration(
                    Collections.emptyList());
            cfg.sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN;
            pc = factory.createPeerConnection(cfg, new PeerConnection.Observer() {
                @Override public void onSignalingChange(PeerConnection.SignalingState s) {}
                @Override public void onIceConnectionChange(PeerConnection.IceConnectionState s) {
                    Log.i(TAG, "ice " + s);
                }
                @Override public void onIceConnectionReceivingChange(boolean b) {}
                @Override public void onIceGatheringChange(PeerConnection.IceGatheringState s) {}
                @Override public void onIceCandidate(IceCandidate c) {}
                @Override public void onIceCandidatesRemoved(IceCandidate[] c) {}
                @Override public void onAddStream(MediaStream s) {}
                @Override public void onRemoveStream(MediaStream s) {}
                @Override public void onDataChannel(DataChannel c) {}
                @Override public void onRenegotiationNeeded() {}
            });
            if (pc == null) throw new IOException("PeerConnection 建不起来");
            DataChannel.Init init = new DataChannel.Init();
            init.ordered = true;
            init.maxRetransmits = 3;
            init.protocol = "tcp";
            dc = pc.createDataChannel("chat", init);
            dc.registerObserver(new DataChannel.Observer() {
                @Override public void onBufferedAmountChange(long l) {}
                @Override public void onStateChange() {
                    DataChannel ch = dc;
                    if (ch == null) return;
                    Log.i(TAG, "dc " + ch.state());
                    // 没有走球机网页那路 WebRTC 画面时，interaction 可能不来。
                    // 通道一开就回 OK，否则喇叭那边不会接麦。
                    if (ch.state() == DataChannel.State.OPEN) {
                        sendText("OK\0");
                        if (on.get()) sendText("status=SoundOn\0");
                    }
                }
                @Override public void onMessage(DataChannel.Buffer buf) {
                    if (buf == null || buf.data == null) return;
                    byte[] raw = new byte[buf.data.remaining()];
                    buf.data.get(raw);
                    if (buf.binary) return;
                    String s = new String(raw, StandardCharsets.UTF_8);
                    if (s.startsWith("interaction")) {
                        sendText("OK\0");
                        if (on.get()) sendText("status=SoundOn\0");
                    }
                }
            });
            // 球机网页 play() 音视频都订。喇叭只认这条完整会话上的 chat。
            pc.addTransceiver(org.webrtc.MediaStreamTrack.MediaType.MEDIA_TYPE_AUDIO,
                    new RtpTransceiver.RtpTransceiverInit(
                            RtpTransceiver.RtpTransceiverDirection.RECV_ONLY));
            pc.addTransceiver(org.webrtc.MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO,
                    new RtpTransceiver.RtpTransceiverInit(
                            RtpTransceiver.RtpTransceiverDirection.RECV_ONLY));
            SessionDescription offer = createOffer();
            String sdp = rewriteSctp(offer.description);
            offer = new SessionDescription(offer.type, sdp);
            setLocal(offer);
            waitIceGather();
            SessionDescription local = pc.getLocalDescription();
            String offerSdp = local != null ? local.description : sdp;
            String answer = postOffer(host, offerSdp);
            setRemote(new SessionDescription(SessionDescription.Type.ANSWER, answer));
            waitIceConnected();
            if (!waitDcOpen(2500)) {
                Log.w(TAG, "datachannel 没开");
            }
        } finally {
            unpin(app);
        }
        if (gen != talkGen.get()) {
            teardownSession();
            return;
        }
        sessionReady.set(true);
        if (startMic) beginTalk();
    }

    private synchronized void ensureFactory(Context ctx) {
        if (factory != null) return;
        PeerConnectionFactory.initialize(
                PeerConnectionFactory.InitializationOptions.builder(ctx)
                        .createInitializationOptions());
        PeerConnectionFactory.Options opts = new PeerConnectionFactory.Options();
        opts.networkIgnoreMask = PeerConnectionFactory.Options.ADAPTER_TYPE_CELLULAR;
        adm = JavaAudioDeviceModule.builder(ctx)
                .setUseHardwareAcousticEchoCanceler(true)
                .setUseHardwareNoiseSuppressor(true)
                .createAudioDeviceModule();
        // WebRTC 订了 recvonly 音轨会自己从喇叭放，和 RTSP 叠在一起就是回声。
        adm.setSpeakerMute(true);
        adm.setMicrophoneMute(true);
        factory = PeerConnectionFactory.builder()
                .setOptions(opts)
                .setAudioDeviceModule(adm)
                .setVideoEncoderFactory(new SoftwareVideoEncoderFactory())
                .setVideoDecoderFactory(new SoftwareVideoDecoderFactory())
                .createPeerConnectionFactory();
    }

    /** 只松绑。对讲再整进程钉 MESH，画中画 CGI 和 RTSP 会一起断。 */
    private void unpin(Context ctx) {
        if (Build.VERSION.SDK_INT < 23 || ctx == null) return;
        ConnectivityManager cm = (ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm == null) return;
        try {
            cm.bindProcessToNetwork(null);
        } catch (Exception e) {
            Log.w(TAG, "unpin", e);
        }
    }

    private void waitIceConnected() {
        long until = System.currentTimeMillis() + 3000;
        while (pc != null && System.currentTimeMillis() < until) {
            PeerConnection.IceConnectionState s = pc.iceConnectionState();
            if (s == PeerConnection.IceConnectionState.CONNECTED
                    || s == PeerConnection.IceConnectionState.COMPLETED) {
                return;
            }
            try { Thread.sleep(40); } catch (InterruptedException e) { return; }
        }
    }

    private boolean waitDcOpen(long ms) {
        long until = System.currentTimeMillis() + ms;
        while (System.currentTimeMillis() < until) {
            DataChannel ch = dc;
            if (ch != null && ch.state() == DataChannel.State.OPEN) return true;
            try { Thread.sleep(40); } catch (InterruptedException e) { return false; }
        }
        return dc != null && dc.state() == DataChannel.State.OPEN;
    }

    private void waitIceGather() {
        long until = System.currentTimeMillis() + 2500;
        while (pc != null && System.currentTimeMillis() < until) {
            if (pc.iceGatheringState() == PeerConnection.IceGatheringState.COMPLETE) {
                return;
            }
            try { Thread.sleep(40); } catch (InterruptedException e) { return; }
        }
    }

    private static String rewriteSctp(String sdp) {
        if (sdp == null) return "";
        int port = 40000 + new SecureRandom().nextInt(1000);
        return sdp.replaceAll("sctp-port:\\d+", "sctp-port:" + port);
    }

    private SessionDescription createOffer() throws Exception {
        WaitSdp wait = new WaitSdp();
        pc.createOffer(wait, new MediaConstraints());
        return wait.await();
    }

    private void setLocal(SessionDescription sdp) throws Exception {
        WaitSdp wait = new WaitSdp();
        pc.setLocalDescription(wait, sdp);
        wait.await();
    }

    private void setRemote(SessionDescription sdp) throws Exception {
        WaitSdp wait = new WaitSdp();
        pc.setRemoteDescription(wait, sdp);
        wait.await();
    }

    private String postOffer(String host, String sdp) throws Exception {
        OkHttpClient.Builder b = new OkHttpClient.Builder()
                .connectTimeout(3, TimeUnit.SECONDS)
                .readTimeout(4, TimeUnit.SECONDS)
                .writeTimeout(4, TimeUnit.SECONDS);
        trustAll(b);
        Network net = RadioLink.get().meshNetwork();
        if (net != null) b.socketFactory(net.getSocketFactory());
        String api = "https://" + host + ":1988/rtc/v1/play/";
        OkHttpClient http = b.build();
        // 球机网页就是 raw SDP 后面跟一个 }，Content-Type: text/plain。
        Request raw = new Request.Builder()
                .url(api)
                .post(RequestBody.create(sdp + "}", MediaType.parse("text/plain")))
                .build();
        try (Response res = http.newCall(raw).execute()) {
            String body = res.body() != null ? res.body().string() : "";
            String ans = readAnswer(body);
            if (!ans.isEmpty()) return ans;
        } catch (Exception e) {
            Log.w(TAG, "sdp-plain", e);
        }
        JSONObject payload = new JSONObject();
        payload.put("api", api);
        payload.put("streamurl", "webrtc://" + host + ":1988/live/livestream");
        payload.put("sdp", sdp);
        Request json = new Request.Builder()
                .url(api)
                .post(RequestBody.create(payload.toString(), MediaType.parse("application/json")))
                .build();
        try (Response res = http.newCall(json).execute()) {
            String body = res.body() != null ? res.body().string() : "";
            String ans = readAnswer(body);
            if (!ans.isEmpty()) return ans;
            throw new IOException("球机对讲协商失败 " + body);
        }
    }

    private static String readAnswer(String body) {
        if (body == null || body.isEmpty()) return "";
        try {
            JSONObject o = new JSONObject(body);
            if (o.optInt("code", -1) != 0) return "";
            return o.optString("sdp", "");
        } catch (Exception e) {
            return "";
        }
    }

    private synchronized void startMic() {
        if (rec != null) return;
        for (int hz : REC_RATES) {
            int min = AudioRecord.getMinBufferSize(hz,
                    AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
            if (min <= 0) continue;
            int chunk = Math.max(min, (hz / SAMPLE_HZ) * FRAME_SAMPLES * 4);
            AudioRecord cand = new AudioRecord(MediaRecorder.AudioSource.VOICE_COMMUNICATION,
                    hz, AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT, chunk);
            if (cand.getState() == AudioRecord.STATE_INITIALIZED) {
                rec = cand;
                recHz = hz;
                break;
            }
            cand.release();
        }
        if (rec == null) throw new IllegalStateException("麦克风打不开");
        rec.startRecording();
        recThread = new Thread(this::pumpMic, "ptz-mic");
        recThread.start();
    }

    private void pumpMic() {
        int chunk = FRAME_SAMPLES * Math.max(1, recHz / SAMPLE_HZ);
        short[] raw = new short[chunk];
        short[] pcm = new short[FRAME_SAMPLES];
        byte[] alaw = new byte[FRAME_SAMPLES];
        while (on.get()) {
            AudioRecord r = rec;
            if (r == null) break;
            int n = r.read(raw, 0, raw.length);
            if (n < raw.length) continue;
            DataChannel ch = dc;
            // 麦比数据通道先开。通道还没建好就 break，后面 beginTalk 看到 rec
            // 还在，不会再起线程，球机整段对讲都是静音。
            if (ch == null || ch.state() != DataChannel.State.OPEN) continue;
            downsample(raw, recHz, pcm);
            encodeAlaw(pcm, alaw);
            ch.send(new DataChannel.Buffer(ByteBuffer.wrap(Arrays.copyOf(alaw, alaw.length)), true));
        }
    }

    private static void downsample(short[] in, int inHz, short[] out) {
        if (inHz == SAMPLE_HZ) {
            System.arraycopy(in, 0, out, 0, Math.min(in.length, out.length));
            return;
        }
        for (int i = 0; i < out.length; i++) {
            int idx = (int) ((long) i * inHz / SAMPLE_HZ);
            if (idx >= in.length) idx = in.length - 1;
            out[i] = in[idx];
        }
    }

    private void sendText(String s) {
        DataChannel ch = dc;
        if (ch == null || ch.state() != DataChannel.State.OPEN) return;
        try {
            ch.send(new DataChannel.Buffer(
                    ByteBuffer.wrap(s.getBytes(StandardCharsets.UTF_8)), false));
        } catch (Exception e) {
            Log.w(TAG, "sendText", e);
        }
    }

    // G.711 A-law，和球机网页 alawmulaw.js 同一套。
    static void encodeAlaw(short[] pcm, byte[] out) {
        for (int i = 0; i < pcm.length; i++) {
            int val = pcm[i];
            int sign = (val >> 8) & 0x80;
            if (sign != 0) val = -val;
            if (val > 32635) val = 32635;
            int exp = 7;
            for (int mask = 0x4000; (val & mask) == 0 && exp > 0; mask >>= 1) exp--;
            int mant = (exp == 0) ? ((val >> 4) & 0x0F) : ((val >> (exp + 3)) & 0x0F);
            out[i] = (byte) ((sign | (exp << 4) | mant) ^ 0x55);
        }
    }

    private static void trustAll(OkHttpClient.Builder b) throws Exception {
        X509TrustManager tm = new X509TrustManager() {
            @Override public void checkClientTrusted(X509Certificate[] c, String a) {}
            @Override public void checkServerTrusted(X509Certificate[] c, String a) {}
            @Override public X509Certificate[] getAcceptedIssuers() {
                return new X509Certificate[0];
            }
        };
        SSLContext ctx = SSLContext.getInstance("TLS");
        ctx.init(null, new TrustManager[]{tm}, new SecureRandom());
        b.sslSocketFactory(ctx.getSocketFactory(), tm);
        b.hostnameVerifier((h, s) -> true);
    }

    private static final class WaitSdp implements SdpObserver {
        private final CountDownLatch done = new CountDownLatch(1);
        private SessionDescription sdp;
        private String err;

        @Override public void onCreateSuccess(SessionDescription s) {
            sdp = s;
            done.countDown();
        }
        @Override public void onSetSuccess() { done.countDown(); }
        @Override public void onCreateFailure(String e) { err = e; done.countDown(); }
        @Override public void onSetFailure(String e) { err = e; done.countDown(); }

        @NonNull SessionDescription await() throws Exception {
            if (!done.await(4, TimeUnit.SECONDS)) throw new IOException("SDP 超时");
            if (err != null) throw new IOException(err);
            return sdp != null ? sdp : new SessionDescription(SessionDescription.Type.OFFER, "");
        }
    }
}
