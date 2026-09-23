package com.dogx30.control;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.audiofx.AcousticEchoCanceler;
import android.media.audiofx.AutomaticGainControl;
import android.media.audiofx.NoiseSuppressor;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.media.MediaRecorder;
import android.net.ConnectivityManager;
import android.net.Network;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.speech.tts.TextToSpeech;
import android.speech.tts.UtteranceProgressListener;
import android.util.Log;
import android.widget.Toast;

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

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.security.cert.X509Certificate;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Random;
import java.util.concurrent.CopyOnWriteArrayList;
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
 * 回 OK\\0，再按 audioType 推音频。网页默认是 AAC（AudioFormat=37），
 * 只有明确写 g711a/g711u 才走 G.711。App 以前默认 G.711A，PC 能喊、App 静音。
 *
 * App 画面仍走 RTSP。左旋 CH11 开麦并调音量，右旋 CH12 听球并调音量。
 * 人说话时压低球机返回声。球机网页开麦前先发 status=SoundOn。
 */
final class CameraTalk {
    private static final String TAG = "CameraTalk";
    static final int REQ_MIC = 71;
    private static final int SAMPLE_HZ = 8000;
    private static final int FRAME_SAMPLES = 320; // 40 ms G.711
    private static final int AAC_SAMPLES = 1024; // AAC-LC 一帧
    private static final String GO_AHEAD = "请您讲";
    private static final int VAD_ON = 400;
    /** 只拦空包，不再用来压听音。听球漏进麦时略抬一点。 */
    private static final int VAD_ON_LISTEN = 700;
    private static final int VAD_ON_FRAMES = 2;
    private static final long VAD_HANG_MS = 400;
    private static final int[] REC_RATES = { 48000, 44100, 16000, 8000 };
    // VOICE_COMMUNICATION 会跟 WebRTC ADM 抢同一路麦，ADM 静音后 AudioRecord 仍在转、状态栏有麦标，球机却是静音。
    private static final int[] REC_SOURCES = {
            MediaRecorder.AudioSource.CAMCORDER,
            MediaRecorder.AudioSource.MIC,
            MediaRecorder.AudioSource.UNPROCESSED,
            MediaRecorder.AudioSource.DEFAULT,
    };
    private static final CameraTalk INST = new CameraTalk();
    private static final ExecutorService TALK = Executors.newSingleThreadExecutor();

    static CameraTalk get() {
        return INST;
    }

    private final AtomicBoolean on = new AtomicBoolean(false);
    private final AtomicBoolean sessionReady = new AtomicBoolean(false);
    private final AtomicBoolean armed = new AtomicBoolean(false);
    private final AtomicBoolean starting = new AtomicBoolean(false);
    private final AtomicBoolean wantStart = new AtomicBoolean(false);
    private final AtomicInteger talkGen = new AtomicInteger(0);
    private PeerConnectionFactory factory;
    private JavaAudioDeviceModule adm;
    private PeerConnection pc;
    private DataChannel dc;
    private final List<DataChannel> channels = new CopyOnWriteArrayList<>();
    private final Object sendLock = new Object();
    private final AtomicBoolean gotInteraction = new AtomicBoolean(false);
    private final AtomicBoolean okSent = new AtomicBoolean(false);
    private volatile String audioType = "aac";
    private volatile float outGain = 1f;
    private MediaCodec aacEnc;
    private long aacPtsUs;
    private IceCandidate hostIce;
    private AudioRecord rec;
    private AcousticEchoCanceler aec;
    private NoiseSuppressor ns;
    private AutomaticGainControl agc;
    private Thread recThread;
    private int recHz = 48000;
    private int lp1;
    private String pendingHost = "";
    private NativeVideo video;
    private Tts appTts;
    private Context appCtx;
    private long startedAt;
    private final AtomicBoolean nativeOwned = new AtomicBoolean(false);
    private final AtomicBoolean ending = new AtomicBoolean(false);
    private final AtomicBoolean cueCancel = new AtomicBoolean(false);
    private TextToSpeech cueTts;
    private final AtomicBoolean cueReady = new AtomicBoolean(false);
    private final Handler soundOn = new Handler(Looper.getMainLooper());
    private final Runnable keepSoundOn = new Runnable() {
        @Override public void run() {
            if (!on.get()) return;
            // 网页 playerSendCtrInfo 只发这一句，多变体会让球机解析失败。
            sendText("status=SoundOn\0");
            soundOn.postDelayed(this, 1000);
        }
    };

    void attachVideo(NativeVideo v) {
        video = v;
    }

    void attachTts(Tts t) {
        appTts = t;
    }

    /** 保留给测试；开球不再预建会话，否则状态栏会提前出麦标。 */
    void prepare(Context ctx, String host) {
        String h = host == null ? "" : host.trim();
        if (h.isEmpty()) h = "192.168.10.168";
        pendingHost = h;
    }

    boolean isOn() {
        return on.get() || starting.get() || ending.get();
    }

    /** R1：开麦或关麦。听球由 HOME 管。 */
    void toggle(Activity act, String host) {
        if (isOn()) {
            stopFromUser();
            return;
        }
        startFromUser(act, host);
    }

    void startFromUser(Activity act, String host) {
        nativeOwned.set(true);
        start(act, host);
    }

    void stopFromUser() {
        nativeOwned.set(false);
        stop(true, true);
    }

    void start(Activity act, String host) {
        String h = host == null ? "" : host.trim();
        if (h.isEmpty()) h = "192.168.10.168";
        pendingHost = h;
        if (on.get() || starting.get() || ending.get()) return;
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
        startedAt = System.currentTimeMillis();
        NativeVideo v = video;
        if (v != null) v.setTalking(true);
        if (!starting.compareAndSet(false, true)) return;
        final String dest = h;
        final Context app = act.getApplicationContext();
        appCtx = app;
        final int gen = talkGen.incrementAndGet();
        TALK.execute(() -> {
            ensureCueTts(app);
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

    /** R1 再按：关麦。听球由 HOME 管，不在这里开关。 */
    void stop() {
        stop(false, true);
    }

    void stop(boolean force) {
        stop(force, false);
    }

    void stop(boolean force, boolean cue) {
        if (ending.get()) {
            if (!cue) cueCancel.set(true);
            return;
        }
        if (!force && nativeOwned.get()) {
            Log.i(TAG, "ignore js stop");
            return;
        }
        if (!force && on.get() && System.currentTimeMillis() - startedAt < 800) {
            Log.i(TAG, "ignore stop bounce");
            return;
        }
        wantStart.set(false);
        starting.set(false);
        talkGen.incrementAndGet();
        on.set(false);
        cueCancel.set(false);
        ending.set(true);
        NativeVideo v = video;
        if (v != null) {
            v.setSpeakDuck(false);
            v.setAdmPlaying(false);
            v.setTalking(false);
        }
        TALK.execute(() -> {
            stopMic();
            shutdownTalk();
            ending.set(false);
        });
    }

    void release() {
        cueCancel.set(true);
        nativeOwned.set(false);
        stop(true, false);
        TALK.execute(this::disposeCueTts);
    }

    private void shutdownTalk() {
        soundOn.removeCallbacks(keepSoundOn);
        stopMic();
        releaseAacEnc();
        if (adm != null) {
            try { adm.setSpeakerMute(true); } catch (Exception ignored) {}
        }
        teardownPeer();
        disposeFactory();
        unpin(appCtx);
    }

    private void ensureCueTts(Context ctx) {
        if (cueTts != null || ctx == null) return;
        CountDownLatch ready = new CountDownLatch(1);
        cueTts = new TextToSpeech(ctx.getApplicationContext(), code -> {
            try {
                if (code != TextToSpeech.SUCCESS) return;
                int lang = cueTts.setLanguage(Locale.CHINA);
                if (lang == TextToSpeech.LANG_MISSING_DATA
                        || lang == TextToSpeech.LANG_NOT_SUPPORTED) {
                    lang = cueTts.setLanguage(Locale.SIMPLIFIED_CHINESE);
                }
                if (lang != TextToSpeech.LANG_MISSING_DATA
                        && lang != TextToSpeech.LANG_NOT_SUPPORTED) {
                    cueTts.setSpeechRate(1.05f);
                    cueReady.set(true);
                }
            } finally {
                ready.countDown();
            }
        });
        try {
            ready.await(2, TimeUnit.SECONDS);
        } catch (InterruptedException ignored) {}
    }

    private void disposeCueTts() {
        cueReady.set(false);
        TextToSpeech t = cueTts;
        cueTts = null;
        if (t == null) return;
        try { t.stop(); } catch (Exception ignored) {}
        try { t.shutdown(); } catch (Exception ignored) {}
    }

    /** 听音已开之后，把「请您讲」推到球机喇叭。 */
    private void playGoAhead() {
        if (cueCancel.get() || !anyDcOpen()) return;
        sendCtrl("status=SoundOn");
        try { Thread.sleep(250); } catch (InterruptedException e) { return; }
        if (cueCancel.get() || !anyDcOpen()) return;
        short[] pcm = synthGoAhead();
        if (pcm == null || pcm.length == 0) return;
        sendPcm8k(pcm);
        try { Thread.sleep(200); } catch (InterruptedException ignored) {}
    }

    private short[] synthGoAhead() {
        Context ctx = appCtx;
        Tts voice = appTts;
        if (ctx == null) return null;
        File wav = new File(ctx.getCacheDir(), "talk-go.wav");
        CountDownLatch done = new CountDownLatch(1);
        if (voice != null && voice.synthToFile(GO_AHEAD, wav, done::countDown)) {
            try {
                if (!done.await(4, TimeUnit.SECONDS) || !wav.exists()) return fallbackCueSynth(wav);
            } catch (InterruptedException e) {
                return null;
            }
            try {
                return wavToPcm8k(wav);
            } catch (Exception e) {
                Log.w(TAG, "go wav", e);
            }
        }
        return fallbackCueSynth(wav);
    }

    private short[] fallbackCueSynth(File wav) {
        TextToSpeech tts = cueTts;
        if (!cueReady.get() || tts == null) return null;
        CountDownLatch done = new CountDownLatch(1);
        AtomicBoolean ok = new AtomicBoolean(false);
        tts.setOnUtteranceProgressListener(new UtteranceProgressListener() {
            @Override public void onStart(String id) {}
            @Override public void onDone(String id) {
                ok.set(true);
                done.countDown();
            }
            @Override public void onError(String id) { done.countDown(); }
        });
        int r = tts.synthesizeToFile(GO_AHEAD, new Bundle(), wav, "goahead");
        if (r != TextToSpeech.SUCCESS) return null;
        try {
            if (!done.await(4, TimeUnit.SECONDS) || !ok.get()) return null;
        } catch (InterruptedException e) {
            return null;
        }
        try {
            return wavToPcm8k(wav);
        } catch (Exception e) {
            Log.w(TAG, "go wav", e);
            return null;
        }
    }

    private void sendPcm8k(short[] pcm) {
        if (pcm == null || pcm.length == 0) return;
        if (wantsAac()) {
            short[] aac = new short[AAC_SAMPLES];
            int fill = 0;
            int src = 0;
            while (src < pcm.length && anyDcOpen() && !cueCancel.get()) {
                int take = Math.min(pcm.length - src, AAC_SAMPLES - fill);
                System.arraycopy(pcm, src, aac, fill, take);
                fill += take;
                src += take;
                if (fill < AAC_SAMPLES) continue;
                fill = 0;
                encodeAndSendAac(aac, true);
                try { Thread.sleep(AAC_SAMPLES * 1000L / SAMPLE_HZ); }
                catch (InterruptedException e) { return; }
            }
            if (fill > 0 && anyDcOpen() && !cueCancel.get()) {
                Arrays.fill(aac, fill, AAC_SAMPLES, (short) 0);
                encodeAndSendAac(aac, true);
            }
            return;
        }
        short[] frame = new short[FRAME_SAMPLES];
        byte[] g711 = new byte[FRAME_SAMPLES];
        int i = 0;
        while (i < pcm.length && anyDcOpen() && !cueCancel.get()) {
            int n = Math.min(FRAME_SAMPLES, pcm.length - i);
            Arrays.fill(frame, (short) 0);
            System.arraycopy(pcm, i, frame, 0, n);
            i += n;
            encodeTalk(frame, g711);
            sendBin(g711);
            try { Thread.sleep(FRAME_SAMPLES * 1000L / SAMPLE_HZ); }
            catch (InterruptedException e) { return; }
        }
    }

    private static short[] wavToPcm8k(File f) throws IOException {
        long n = f.length();
        if (n < 44 || n > 2_000_000) return null;
        byte[] all = new byte[(int) n];
        try (RandomAccessFile raf = new RandomAccessFile(f, "r")) {
            raf.readFully(all);
        }
        int pos = 12;
        int channels = 1;
        int hz = 16000;
        int bits = 16;
        int dataOff = -1;
        int dataLen = 0;
        while (pos + 8 <= all.length) {
            String id = new String(all, pos, 4, StandardCharsets.US_ASCII);
            int sz = u32le(all, pos + 4);
            if ("fmt ".equals(id) && pos + 24 <= all.length) {
                channels = Math.max(1, u16le(all, pos + 10));
                hz = u32le(all, pos + 12);
                bits = u16le(all, pos + 22);
            } else if ("data".equals(id)) {
                dataOff = pos + 8;
                dataLen = Math.min(sz, all.length - dataOff);
                break;
            }
            pos += 8 + sz;
            if ((sz & 1) != 0) pos++;
        }
        if (bits != 16 || dataOff < 0 || dataLen < 2) return null;
        int samples = dataLen / (2 * channels);
        short[] mono = new short[samples];
        ByteBuffer bb = ByteBuffer.wrap(all, dataOff, dataLen).order(ByteOrder.LITTLE_ENDIAN);
        for (int i = 0; i < samples; i++) {
            int sum = 0;
            for (int c = 0; c < channels; c++) {
                if (bb.remaining() < 2) break;
                sum += bb.getShort();
            }
            mono[i] = (short) (sum / channels);
        }
        if (hz == SAMPLE_HZ) return mono;
        if (hz <= 0) return null;
        short[] out = new short[Math.max(1, (int) ((long) samples * SAMPLE_HZ / hz))];
        downsample(mono, hz, out);
        return out;
    }

    private static int u16le(byte[] b, int i) {
        return (b[i] & 0xFF) | ((b[i + 1] & 0xFF) << 8);
    }

    private static int u32le(byte[] b, int i) {
        return (b[i] & 0xFF) | ((b[i + 1] & 0xFF) << 8)
                | ((b[i + 2] & 0xFF) << 16) | ((b[i + 3] & 0xFF) << 24);
    }

    /** 重试时只拆连接，工厂留下。在网页线程里 dispose 工厂会卡死 WebView。 */
    private void teardownSession() {
        teardownPeer();
    }

    private void teardownPeer() {
        sessionReady.set(false);
        armed.set(false);
        gotInteraction.set(false);
        okSent.set(false);
        audioType = "aac";
        hostIce = null;
        List<DataChannel> copy = new java.util.ArrayList<>(channels);
        channels.clear();
        dc = null;
        for (DataChannel ch : copy) {
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
        // 球机网页时序：等 interaction → 只回 OK\0 → 再 status=SoundOn\0 → Talkback 推音频。
        // 自己伪造 interaction 球机不会接喇叭。
        if (!okSent.get()) sendCtrl("OK");
        sendCtrl("status=SoundOn");
        try { Thread.sleep(200); } catch (InterruptedException e) { return; }
        armed.set(true);
        soundOn.removeCallbacks(keepSoundOn);
        // 网页只发一次 SoundOn。每秒再发，有的球机会每秒滴一声。
        // ADM 喇叭保持静音：WebRTC 音轨经常是空的，再把 RTSP 让开就整段没声。
        // 球机返回声只走 RTSP。ADM 只为建对讲数据通道，麦口让给 AudioRecord。
        if (adm != null) {
            try { adm.setSpeakerMute(true); } catch (Exception ignored) {}
            try { adm.setMicrophoneMute(true); } catch (Exception ignored) {}
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
        releaseMicFx();
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

    private void attachMicFx(AudioRecord r) {
        int id = r.getAudioSessionId();
        if (AcousticEchoCanceler.isAvailable()) {
            aec = AcousticEchoCanceler.create(id);
            if (aec != null) {
                aec.setEnabled(true);
                Log.i(TAG, "aec on");
            }
        }
        if (NoiseSuppressor.isAvailable()) {
            ns = NoiseSuppressor.create(id);
            if (ns != null) ns.setEnabled(true);
        }
        if (AutomaticGainControl.isAvailable()) {
            agc = AutomaticGainControl.create(id);
            // 关掉硬件 AGC，左旋音量才调得动。开着会把拧小的音量再抬回去。
            if (agc != null) agc.setEnabled(false);
        }
    }

    private void releaseMicFx() {
        if (aec != null) {
            try { aec.release(); } catch (Exception ignored) {}
            aec = null;
        }
        if (ns != null) {
            try { ns.release(); } catch (Exception ignored) {}
            ns = null;
        }
        if (agc != null) {
            try { agc.release(); } catch (Exception ignored) {}
            agc = null;
        }
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
            if (!on.get()) unpin(app);
        }
    }

    private void connectPlay(Context app, String host, boolean startMic, int gen)
            throws Exception {
        pinMesh(app);
        gotInteraction.set(false);
        okSent.set(false);
        audioType = "aac";
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
                @Override public void onIceCandidate(IceCandidate c) {
                    if (c == null || c.sdp == null) return;
                    if (c.sdp.contains("typ host") && c.sdp.contains("192.168.10.")) {
                        hostIce = c;
                    }
                }
                @Override public void onIceCandidatesRemoved(IceCandidate[] c) {}
                @Override public void onAddStream(MediaStream s) {}
                @Override public void onRemoveStream(MediaStream s) {}
                @Override public void onDataChannel(DataChannel c) {
                    Log.i(TAG, "in-dc " + (c != null ? c.label() : "null"));
                    attachDc(c);
                }
                @Override public void onRenegotiationNeeded() {}
            });
            if (pc == null) throw new IOException("PeerConnection 建不起来");
            DataChannel.Init init = new DataChannel.Init();
            init.ordered = true;
            init.maxRetransmits = 3;
            init.protocol = "tcp";
            attachDc(pc.createDataChannel("chat", init));
            // 球机网页 play() 音视频都订。喇叭只认这条完整会话上的 chat。
            pc.addTransceiver(org.webrtc.MediaStreamTrack.MediaType.MEDIA_TYPE_AUDIO,
                    new RtpTransceiver.RtpTransceiverInit(
                            RtpTransceiver.RtpTransceiverDirection.RECV_ONLY));
            pc.addTransceiver(org.webrtc.MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO,
                    new RtpTransceiver.RtpTransceiverInit(
                            RtpTransceiver.RtpTransceiverDirection.RECV_ONLY));
            SessionDescription offer = createOffer();
            try {
                setLocal(new SessionDescription(SessionDescription.Type.OFFER,
                        rewriteSctp(offer.description)));
            } catch (Exception e) {
                Log.w(TAG, "sctp rewrite", e);
                setLocal(offer);
            }
            waitIceGather();
            SessionDescription local = pc.getLocalDescription();
            String offerSdp = local != null ? local.description : offer.description;
            String answer = postOffer(host, offerSdp);
            setRemote(new SessionDescription(SessionDescription.Type.ANSWER, answer));
            IceCandidate hc = hostIce;
            if (hc != null) {
                try { pc.addIceCandidate(hc); } catch (Exception e) {
                    Log.w(TAG, "hostIce", e);
                }
            }
            waitIceConnected();
            if (!waitDcOpen(4000)) {
                Log.w(TAG, "datachannel 没开");
            }
            if (!waitInteraction(4000)) {
                Log.w(TAG, "no interaction " + audioType);
            }
        } catch (Exception e) {
            unpin(app);
            throw e;
        }
        if (gen != talkGen.get()) {
            teardownSession();
            unpin(app);
            return;
        }
        sessionReady.set(true);
        if (startMic) beginTalk();
    }

    private void toast(String msg) {
        Context ctx = appCtx;
        if (ctx == null) return;
        soundOn.post(() -> Toast.makeText(ctx, msg, Toast.LENGTH_LONG).show());
    }

    private void attachDc(DataChannel ch) {
        if (ch == null) return;
        channels.add(ch);
        dc = ch;
        ch.registerObserver(new DataChannel.Observer() {
            @Override public void onBufferedAmountChange(long l) {}
            @Override public void onStateChange() {
                Log.i(TAG, "dc " + ch.label() + " " + ch.state());
            }
            @Override public void onMessage(DataChannel.Buffer buf) {
                onDcMessage(ch, buf);
            }
        });
    }

    private void onDcMessage(DataChannel ch, DataChannel.Buffer buf) {
        if (buf == null || buf.data == null) return;
        byte[] raw = new byte[buf.data.remaining()];
        buf.data.get(raw);
        // 球机固件常把 interaction 当二进制（带 \\0）送来，丢掉就永远回不了 OK。
        String s = new String(raw, StandardCharsets.UTF_8);
        Log.i(TAG, "dc in " + (buf.binary ? "bin " : "txt ") + s.replace('\0', ' '));
        if (!s.contains("interaction")) return;
        parseInteraction(s);
        dc = ch;
        gotInteraction.set(true);
        sendCtrl("OK");
    }

    private void parseInteraction(String s) {
        // 网页：info[2].split(':')[1]，形如 interaction,videoType:h264,audioType:aac,...
        String[] info = s.split(",");
        if (info.length > 2 && info[2].contains(":")) {
            String[] kv = info[2].split(":", 2);
            if (kv.length == 2) {
                audioType = kv[1].trim().replace("\0", "").toLowerCase(Locale.US);
            }
        }
        for (String part : info) {
            int c = part.indexOf(':');
            if (c <= 0) continue;
            String k = part.substring(0, c).trim();
            String v = part.substring(c + 1).trim().replace("\0", "");
            if (k.equalsIgnoreCase("audioType") || k.equalsIgnoreCase("audio")) {
                audioType = v.toLowerCase(Locale.US);
            }
        }
    }

    private boolean waitInteraction(long ms) {
        long until = System.currentTimeMillis() + ms;
        while (System.currentTimeMillis() < until && on.get()) {
            if (gotInteraction.get()) return true;
            try { Thread.sleep(40); } catch (InterruptedException e) { return false; }
        }
        return gotInteraction.get();
    }

    /** 网页 playerSendCtrInfo：字符串后面加 \\0，当文本帧发出。 */
    private void sendCtrl(String data) {
        if ("OK".equals(data)) okSent.set(true);
        sendText(data + "\0");
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

    /** 协商那几秒钉 MESH，ICE 才能打到 10 网球。谈完立刻松，别把 RTSP/CGI 绑死。 */
    private void pinMesh(Context ctx) {
        if (Build.VERSION.SDK_INT < 23 || ctx == null) return;
        Network net = RadioLink.get().meshNetwork();
        if (net == null) return;
        ConnectivityManager cm = (ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm == null) return;
        try {
            cm.bindProcessToNetwork(net);
        } catch (Exception e) {
            Log.w(TAG, "pinMesh", e);
        }
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
            if (anyDcOpen()) return true;
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
        int session = 0;
        NativeVideo nv = video;
        if (nv != null) session = nv.playSessionId();
        outer:
        for (int src : REC_SOURCES) {
            for (int hz : REC_RATES) {
                int min = AudioRecord.getMinBufferSize(hz,
                        AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
                if (min <= 0) continue;
                int chunk = Math.max(min, (hz / SAMPLE_HZ) * FRAME_SAMPLES * 4);
                AudioRecord cand = openRec(src, hz, chunk, session);
                if (cand != null && cand.getState() == AudioRecord.STATE_INITIALIZED) {
                    rec = cand;
                    recHz = hz;
                    lp1 = 0;
                    Log.i(TAG, "mic src=" + src + " hz=" + hz + " sid=" + session);
                    break outer;
                }
                if (cand != null) cand.release();
            }
        }
        if (rec == null) throw new IllegalStateException("麦克风打不开");
        attachMicFx(rec);
        rec.startRecording();
        recThread = new Thread(this::pumpMic, "ptz-mic");
        recThread.start();
    }

    private static AudioRecord openRec(int src, int hz, int chunk, int session) {
        try {
            AudioRecord.Builder b = new AudioRecord.Builder()
                    .setAudioSource(src)
                    .setAudioFormat(new AudioFormat.Builder()
                            .setSampleRate(hz)
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
                            .build())
                    .setBufferSizeInBytes(chunk);
            if (session > 0) {
                try {
                    AudioRecord.Builder.class.getMethod("setAudioSessionId", int.class)
                            .invoke(b, session);
                } catch (Exception ignored) {}
            }
            return b.build();
        } catch (Exception e) {
            try {
                return new AudioRecord(src, hz, AudioFormat.CHANNEL_IN_MONO,
                        AudioFormat.ENCODING_PCM_16BIT, chunk);
            } catch (Exception e2) {
                return null;
            }
        }
    }

    private void pumpMic() {
        int chunk = FRAME_SAMPLES * Math.max(1, recHz / SAMPLE_HZ);
        short[] raw = new short[chunk];
        short[] pcm = new short[FRAME_SAMPLES];
        short[] aacPcm = new short[AAC_SAMPLES];
        int aacFill = 0;
        byte[] g711 = new byte[FRAME_SAMPLES];
        int micPeak = 0;
        int loudFrames = 0;
        long lastLoudAt = 0;
        boolean open = false;
        while (on.get()) {
            AudioRecord r = rec;
            // 通道还没建好就 break，麦线程退出，后面即使 OPEN 也不会再推。
            if (!armed.get() || !anyDcOpen()) {
                if (r != null) r.read(raw, 0, raw.length);
                continue;
            }
            if (r == null) break;
            int n = r.read(raw, 0, raw.length);
            if (n < raw.length) continue;
            downsample(raw, recHz, pcm);
            lowpass8k(pcm);
            NativeVideo v = video;
            if (v != null && v.isListening()) v.echoCancel(pcm);
            // 听为主时不发麦。球机喇叭→球机麦这条环软件 AEC 消不掉。
            if (v != null && v.isListening() && v.isTalking()
                    && v.talkMix() < 0.48f) {
                continue;
            }
            int p = peak(pcm);
            micPeak = Math.max(micPeak, p);
            long now = System.currentTimeMillis();
            int vadOn = (v != null && v.isListening()) ? VAD_ON_LISTEN : VAD_ON;
            if (p >= vadOn) {
                loudFrames++;
                lastLoudAt = now;
            } else {
                loudFrames = 0;
            }
            boolean want = open
                    ? now - lastLoudAt < VAD_HANG_MS
                    : loudFrames >= VAD_ON_FRAMES;
            open = want;
            if (!open) {
                // 没开口不推。空 AAC 在球机上会尖啸。不要喂静音进编码器，会把后续真声音也编哑。
                continue;
            }
            // VAD 用增益前峰值，音量只缩放已开口的 PCM。
            applyOutGain(pcm);
            if (wantsAac()) {
                aacFill = feedAac(pcm, aacPcm, aacFill, true);
            } else {
                encodeTalk(pcm, g711);
                sendBin(g711);
            }
        }
        if (micPeak < 80) Log.w(TAG, "mic peak " + micPeak);
    }

    void setOutGain(float g) {
        if (g < 0.12f) g = 0.12f;
        if (g > 2.8f) g = 2.8f;
        outGain = g;
    }

    private void applyOutGain(short[] pcm) {
        float g = outGain;
        for (int i = 0; i < pcm.length; i++) {
            int v = Math.round(pcm[i] * g);
            if (v > 32767) v = 32767;
            else if (v < -32768) v = -32768;
            pcm[i] = (short) v;
        }
    }

    private static int peak(short[] pcm) {
        int p = 0;
        for (short v : pcm) {
            int a = v < 0 ? -v : v;
            if (a > p) p = a;
        }
        return p;
    }

    private boolean anyDcOpen() {
        for (DataChannel ch : channels) {
            if (ch != null && ch.state() == DataChannel.State.OPEN) return true;
        }
        DataChannel ch = dc;
        return ch != null && ch.state() == DataChannel.State.OPEN;
    }

    // 网页 Talkback：g711a / g711u，其余一律 AAC。
    private boolean wantsAac() {
        String t = audioType == null ? "" : audioType;
        if (t.contains("711a") || t.contains("pcma") || t.contains("alaw")) return false;
        if (t.contains("711u") || t.contains("pcmu") || t.contains("ulaw")
                || t.contains("mulaw")) {
            return false;
        }
        return true;
    }

    private int feedAac(short[] pcm, short[] aacPcm, int fill, boolean send) {
        int src = 0;
        int n = fill;
        while (src < pcm.length) {
            int take = Math.min(pcm.length - src, AAC_SAMPLES - n);
            System.arraycopy(pcm, src, aacPcm, n, take);
            n += take;
            src += take;
            if (n < AAC_SAMPLES) continue;
            n = 0;
            encodeAndSendAac(aacPcm, send);
        }
        return n;
    }

    private void encodeTalk(short[] pcm, byte[] out) {
        String t = audioType == null ? "" : audioType;
        if (t.contains("711u") || t.contains("pcmu") || t.contains("ulaw")
                || t.contains("mulaw")) {
            encodeUlaw(pcm, out);
            return;
        }
        encodeAlaw(pcm, out);
    }

    private void releaseAacEnc() {
        MediaCodec c = aacEnc;
        aacEnc = null;
        aacPtsUs = 0;
        if (c == null) return;
        try { c.stop(); } catch (Exception ignored) {}
        try { c.release(); } catch (Exception ignored) {}
    }

    private boolean ensureAacEnc() {
        if (aacEnc != null) return true;
        String[] names = { null, "OMX.google.aac.encoder", "c2.android.aac.encoder" };
        for (String name : names) {
            MediaCodec c = null;
            try {
                c = name == null
                        ? MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC)
                        : MediaCodec.createByCodecName(name);
                MediaFormat f = MediaFormat.createAudioFormat(
                        MediaFormat.MIMETYPE_AUDIO_AAC, SAMPLE_HZ, 1);
                f.setInteger(MediaFormat.KEY_AAC_PROFILE,
                        MediaCodecInfo.CodecProfileLevel.AACObjectLC);
                f.setInteger(MediaFormat.KEY_BIT_RATE, 16000);
                f.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, AAC_SAMPLES * 2);
                f.setInteger(MediaFormat.KEY_PCM_ENCODING, AudioFormat.ENCODING_PCM_16BIT);
                c.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
                c.start();
                aacEnc = c;
                aacPtsUs = 0;
                Log.i(TAG, "aac enc " + c.getName());
                return true;
            } catch (Exception e) {
                Log.w(TAG, "aac " + name, e);
                if (c != null) {
                    try { c.release(); } catch (Exception ignored) {}
                }
            }
        }
        toast("AAC 编码器打不开");
        return false;
    }

    private int encodeAndSendAac(short[] pcm, boolean send) {
        if (!ensureAacEnc()) return 0;
        try {
            int inIx = aacEnc.dequeueInputBuffer(20000);
            if (inIx < 0) return 0;
            ByteBuffer in = aacEnc.getInputBuffer(inIx);
            if (in == null) return 0;
            in.clear();
            in.order(ByteOrder.nativeOrder());
            for (short s : pcm) {
                if (in.remaining() < 2) break;
                in.putShort(s);
            }
            aacEnc.queueInputBuffer(inIx, 0, pcm.length * 2, aacPtsUs, 0);
            aacPtsUs += pcm.length * 1_000_000L / SAMPLE_HZ;
        } catch (Exception e) {
            Log.w(TAG, "aac in", e);
            return 0;
        }
        int sent = 0;
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        for (int i = 0; i < 8; i++) {
            int ix;
            try {
                ix = aacEnc.dequeueOutputBuffer(info, i == 0 ? 20000 : 0);
            } catch (Exception e) {
                Log.w(TAG, "aac out", e);
                break;
            }
            if (ix == MediaCodec.INFO_TRY_AGAIN_LATER) break;
            if (ix == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED
                    || ix == MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED) {
                continue;
            }
            if (ix < 0) break;
            ByteBuffer out = aacEnc.getOutputBuffer(ix);
            byte[] raw = new byte[Math.max(0, info.size)];
            if (out != null && info.size > 0) {
                out.position(info.offset);
                out.get(raw);
            }
            try { aacEnc.releaseOutputBuffer(ix, false); } catch (Exception ignored) {}
            if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) continue;
            if (raw.length == 0) continue;
            if (send && sendBin(withAdts(raw))) sent++;
        }
        return sent;
    }

    private static byte[] withAdts(byte[] raw) {
        if (raw.length >= 2 && (raw[0] & 0xFF) == 0xFF && (raw[1] & 0xF0) == 0xF0) {
            return raw;
        }
        byte[] h = adtsHeader(raw.length);
        byte[] pkt = new byte[h.length + raw.length];
        System.arraycopy(h, 0, pkt, 0, h.length);
        System.arraycopy(raw, 0, pkt, h.length, raw.length);
        return pkt;
    }

    // AAC-LC / 8 kHz / 单声道。网页 faac 的 outputFormat=1 也是 ADTS。
    private static byte[] adtsHeader(int aacLen) {
        int profile = 2;
        int freqIdx = 11;
        int channels = 1;
        int fullLen = aacLen + 7;
        byte[] h = new byte[7];
        h[0] = (byte) 0xFF;
        h[1] = (byte) 0xF1;
        h[2] = (byte) (((profile - 1) << 6) | (freqIdx << 2) | (channels >> 2));
        h[3] = (byte) (((channels & 3) << 6) | (fullLen >> 11));
        h[4] = (byte) ((fullLen >> 3) & 0xFF);
        h[5] = (byte) (((fullLen & 7) << 5) | 0x1F);
        h[6] = (byte) 0xFC;
        return h;
    }

    private boolean sendBin(byte[] payload) {
        synchronized (sendLock) {
            boolean any = false;
            boolean ok = false;
            for (DataChannel ch : sendTargets()) {
                any = true;
                try {
                    ByteBuffer bb = ByteBuffer.allocateDirect(payload.length);
                    bb.put(payload);
                    bb.flip();
                    if (ch.send(new DataChannel.Buffer(bb, true))) ok = true;
                    else Log.w(TAG, "send false " + ch.label() + " buf=" + ch.bufferedAmount());
                } catch (Exception e) {
                    Log.w(TAG, "sendBin", e);
                }
            }
            return any && ok;
        }
    }

    private List<DataChannel> sendTargets() {
        List<DataChannel> out = new java.util.ArrayList<>();
        for (DataChannel ch : channels) {
            if (ch != null && ch.state() == DataChannel.State.OPEN) out.add(ch);
        }
        DataChannel ch = dc;
        if (ch != null && ch.state() == DataChannel.State.OPEN && !out.contains(ch)) {
            out.add(ch);
        }
        return out;
    }

    private static void downsample(short[] in, int inHz, short[] out) {
        if (inHz == SAMPLE_HZ) {
            System.arraycopy(in, 0, out, 0, Math.min(in.length, out.length));
            return;
        }
        for (int i = 0; i < out.length; i++) {
            int a = (int) ((long) i * inHz / SAMPLE_HZ);
            int b = (int) ((long) (i + 1) * inHz / SAMPLE_HZ);
            if (a >= in.length) a = in.length - 1;
            if (b > in.length) b = in.length;
            if (b <= a) b = Math.min(in.length, a + 1);
            int sum = 0;
            for (int j = a; j < b; j++) sum += in[j];
            out[i] = (short) (sum / (b - a));
        }
    }

    /** 轻低通去毛刺。两级会把说话能量压没，VAD 就永远开不了。 */
    private void lowpass8k(short[] pcm) {
        int a = lp1;
        for (int i = 0; i < pcm.length; i++) {
            a = (a * 3 + pcm[i] * 5) / 8;
            pcm[i] = (short) a;
        }
        lp1 = a;
    }

    private void sendText(String s) {
        byte[] raw = s.getBytes(StandardCharsets.UTF_8);
        synchronized (sendLock) {
            for (DataChannel ch : sendTargets()) {
                try {
                    ch.send(new DataChannel.Buffer(ByteBuffer.wrap(raw), false));
                } catch (Exception e) {
                    Log.w(TAG, "sendText", e);
                }
            }
        }
    }

    private static String rewriteSctp(String sdp) {
        if (sdp == null || !sdp.contains("sctp-port:5000")) return sdp;
        int port = 40000 + new Random().nextInt(1000);
        return sdp.replace("sctp-port:5000", "sctp-port:" + port);
    }

    // G.711 μ-law，和球机网页 alawmulaw.js 同一套。
    static void encodeUlaw(short[] pcm, byte[] out) {
        for (int i = 0; i < pcm.length; i++) {
            int val = pcm[i];
            int sign = (val >> 8) & 0x80;
            if (sign != 0) val = -val;
            val += 132;
            if (val > 32635) val = 32635;
            int exp = 7;
            for (int mask = 0x4000; (val & mask) == 0 && exp > 0; mask >>= 1) exp--;
            int mant = (val >> (exp + 3)) & 0x0F;
            out[i] = (byte) ~(sign | (exp << 4) | mant);
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
