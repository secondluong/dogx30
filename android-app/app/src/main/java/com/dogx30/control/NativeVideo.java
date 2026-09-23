package com.dogx30.control;

import android.content.Context;
import android.media.AudioManager;
import android.net.Network;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.widget.FrameLayout;

import androidx.annotation.Nullable;
import androidx.annotation.OptIn;
import androidx.media3.common.AudioAttributes;
import androidx.media3.common.C;
import androidx.media3.common.MediaItem;
import androidx.media3.common.PlaybackException;
import androidx.media3.common.Player;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.common.audio.AudioProcessor;
import androidx.media3.exoplayer.DefaultLoadControl;
import androidx.media3.exoplayer.DefaultRenderersFactory;
import androidx.media3.exoplayer.ExoPlayer;
import androidx.media3.exoplayer.audio.AudioSink;
import androidx.media3.exoplayer.audio.DefaultAudioSink;
import androidx.media3.exoplayer.audio.TeeAudioProcessor;
import androidx.media3.exoplayer.rtsp.RtspMediaSource;
import androidx.media3.ui.AspectRatioFrameLayout;
import androidx.media3.ui.PlayerView;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;

import javax.net.SocketFactory;

/**
 * 2.4G 链路下的机身相机画面。
 *
 * 为什么要原生解码：2.4G 是**遥控器与狗直连**（接收机挂在机身交换机上），网关开发板
 * 不在这条链路上，所以拿不到它下发的媒体计划，也够不到 MediaMTX —— 网页那条 WebRTC
 * 整条链都不成立。狗自己只提供 RTSP，而 WebView 放不了 RTSP。这就是 2.4G 下一直
 * 没画面的原因，只能由这里解。
 *
 * 两个不显然但必须这么做的点：
 *
 * 1. socket 要绑到 2.4G 那张网卡。G20 射频起来后是虚口 ar_net0，系统
 *    ConnectivityManager 往往看不见它，普通 socket 会被安卓按「默认网络」路由出去，
 *    结果是 Network unreachable —— 和 RadioLink 里 UDP 遇到的完全同一个坑。
 *    所以给播放器塞一个自己的 SocketFactory，逐个 socket 做 SO_BINDTODEVICE。
 * 2. 强制 RTP over TCP。默认先试 UDP、失败再退 TCP，那要多等一个超时；更要紧的是
 *    走 TCP 时 RTSP 和 RTP 复用同一条连接，只要这一条被绑对，整路流就都在 2.4G 上，
 *    不必再去操心 UDP 那几个临时端口有没有绑对。
 */
@OptIn(markerClass = UnstableApi.class)
final class NativeVideo {

    private static final String TAG = "NativeVideo";
    /** RTSP 建连超时。2.4G 窄且抖，给足一点，但不能久到让人以为卡死。 */
    private static final int CONNECT_TIMEOUT_MS = 6000;
    private static final long RTSP_TIMEOUT_MS = 8000;
    /** 断流重试间隔。相机可能比 App 后起来，不能一次失败就再也不试。 */
    private static final long RETRY_MS = 3000;

    // 低延迟：默认的 DefaultLoadControl 是照点播调的，起播前先攒 2.5 秒，
    // 而直播流攒下的每一毫秒都会变成永久延迟 —— 播放器按 1 倍速从起点往后放，
    // 攒进去的那段再也吐不出来。遥控看画面宁可偶尔卡一下，也不要慢一大截。
    private static final int BUFFER_MIN_MS = 200;
    private static final int BUFFER_MAX_MS = 1000;
    private static final int PLAY_AFTER_MS = 100;
    private static final int PLAY_AFTER_REBUFFER_MS = 200;

    // 光把缓冲调小不够：链路抖一下就会攒出一段，之后一直背着走。
    // 所以盯着「已缓冲但还没放」的那段，超了就稍微快放把它排掉。
    // RTSP 直播不能 seek，追不上只能重连（重连即回到实时点）。
    private static final long CATCHUP_MS = 350;
    private static final long RESYNC_MS = 2500;
    private static final float CATCHUP_SPEED = 1.12f;
    private static final long WATCH_MS = 500;

    interface StateListener {
        /**
         * err 为空表示正常出画面。回调在主线程。
         *
         * bufferedMs 是播放器里「已收到但还没放」的那段，也就是本机这一侧贡献的
         * 延迟。它接近 0 却仍然觉得慢，说明延迟在上游（相机转码、链路排队），
         * 客户端再怎么调都没用 —— 这个数就是为了分清这两种情况。
         */
        void onVideoState(boolean playing, String err, long bufferedMs);
    }

    private final PlayerView view;
    private final StateListener listener;
    private final Handler ui = new Handler(Looper.getMainLooper());

    @Nullable private ExoPlayer player;
    private String url = "";
    private boolean wanted;
    /** true：绑 2.4G 网卡。false：走 WiFi，拉板上 MediaMTX 的 10 网转推。 */
    private boolean bindRadio = true;
    private boolean playing;
    /**
     * 先试强制 TCP（见类注释）。但个别 RTSP 服务不支持 TCP interleaved，只认 UDP，
     * 那样死磕 TCP 会一路失败到底。所以每次失败后换一种再试：false 那次是
     * 「先 UDP、收不到再自己退 TCP」，覆盖面更广。用的是哪种会写进报错里。
     */
    private boolean forceTcp = true;
    private boolean listenAudio;
    private volatile boolean talking;
    /** 听球机返回声。右旋 CH12 开关，和左旋开麦分开。 */
    private boolean listening;
    /** 听球音量 0..1，由右旋中间行程给出。 */
    private float listenLevel = 1f;
    /** 左旋 0..1。对讲开着时麦大声则听球变小，麦小则听球变大。 */
    private volatile float talkMix;
    /** 播放和录音共用，硬件 AEC 才对得上参考音。 */
    private int playSessionId;
    /** WebRTC 已在放球机声时，RTSP 音轨静音，避免两条叠成回声。 */
    private boolean admPlaying;
    /** 听音放大。对讲开着时左旋过半听球关掉，打断球机闭环啸叫。 */
    private static final float LISTEN_GAIN = 3.6f;
    /** 左旋高于此值听球静音；低于此值不发麦。 */
    private static final float TALK_MUTE_LISTEN = 0.42f;
    /** 保留符号给测试；听音不再跟 VAD 走。 */
    private static final float LISTEN_DUCK = 1f;
    private boolean speakDuck;
    private volatile float playGain;
    private final SoftwareAec aec = new SoftwareAec();
    private final FarTap farTap = new FarTap();
    private float speed = 1.0f;
    private long lastBufferedMs;

    private final Runnable retry = new Runnable() {
        @Override
        public void run() {
            if (!wanted) return;
            Log.i(TAG, "retry " + url);
            open();
        }
    };

    private final Runnable watch = new Runnable() {
        @Override
        public void run() {
            trimLatency();
            if (wanted && player != null) ui.postDelayed(this, WATCH_MS);
        }
    };

    NativeVideo(PlayerView view, StateListener listener) {
        this.view = view;
        this.listener = listener;
        view.setUseController(false);
        // 和网页里 object-fit: cover 一个意思：铺满格子，宁可裁边也不留黑边、不拉伸。
        view.setResizeMode(AspectRatioFrameLayout.RESIZE_MODE_ZOOM);
    }

    /**
     * 球机切画中画会重开编码（分辨率、SPS 都可能变）。ExoPlayer 这条
     * RTSP 长连接经常继续解旧 GOP，画面就停在切之前；PC 走网关 WebRTC
     * 能跟上。对讲把音轨打开之后更容易卡死解码器。这里拆掉重拉。
     * 对讲刚开时也走这里：从音量 0 拉起来有的机型音轨不醒。
     */
    void refreshAfterPip() {
        refreshStream();
    }

    void refreshStream() {
        ui.removeCallbacks(refreshTask);
        ui.postDelayed(refreshTask, 600);
    }

    private final Runnable refreshTask = () -> {
        if (!wanted || !isBallUrl(url)) return;
        Log.i(TAG, "refresh stream " + url + " talk=" + talking);
        open();
    };

    /** 反复调用同一个地址是空操作，页面切布局时会调很多次。 */
    void start(String rtspUrl) {
        start(rtspUrl, true);
    }

    void start(String rtspUrl, boolean useRadio) {
        final String next = rtspUrl == null ? "" : rtspUrl.trim();
        if (next.isEmpty()) return;
        if (wanted && next.equals(url) && bindRadio == useRadio && player != null) return;
        url = next;
        bindRadio = useRadio;
        wanted = true;
        forceTcp = true;
        // 2.4G 必须先把射频口拉起来再绑 socket。MESH 上看球不要去开射频。
        if (useRadio) RadioLink.get().setEnabled(true);
        open();
    }

    void stop() {
        wanted = false;
        ui.removeCallbacks(retry);
        ui.removeCallbacks(refreshTask);
        release();
        view.setVisibility(View.GONE);
        report(false, "");
    }

    /** 画面画在哪块矩形里，单位是**设备像素**，由网页按 devicePixelRatio 换算后给。 */
    void setRect(int x, int y, int w, int h) {
        if (w <= 0 || h <= 0) return;
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(w, h);
        lp.leftMargin = x;
        lp.topMargin = y;
        view.setLayoutParams(lp);
    }

    /**
     * 切后台：放掉解码器和这条流，但记着「本来是要放的」。
     * 不指望网页的 visibilitychange 一定送到（系统节流时它可能不发），
     * 所以这份意图由原生自己保管，回前台照它恢复。
     */
    void pauseForBackground() {
        ui.removeCallbacks(retry);
        ui.removeCallbacks(refreshTask);
        release();
        view.setVisibility(View.GONE);
    }

    void resumeIfWanted() {
        if (wanted && player == null) open();
    }

    void setTalking(boolean on) {
        talking = on;
        if (!on) {
            admPlaying = false;
            speakDuck = false;
        }
        // 关麦不再重拉：音轨一直在解，只改音量。重拉会黑一下，后半句也听丢。
        ui.post(() -> applyAudio(player));
    }

    /** 旧接口还在。听音改跟左旋交叉，不再认人声压听。 */
    void setSpeakDuck(boolean duck) {
        speakDuck = duck;
    }

    void setTalkMix(float n) {
        float v = Math.max(0f, Math.min(1f, n));
        if (Math.abs(v - talkMix) < 0.01f) return;
        talkMix = v;
        ui.post(() -> applyAudio(player));
    }

    float talkMix() {
        return talkMix;
    }

    boolean isTalking() {
        return talking;
    }

    void echoCancel(short[] pcm) {
        aec.process(pcm);
    }

    int playSessionId() {
        if (playSessionId == 0) {
            AudioManager am = (AudioManager) view.getContext()
                    .getSystemService(Context.AUDIO_SERVICE);
            if (am != null && Build.VERSION.SDK_INT >= 21) {
                playSessionId = am.generateAudioSessionId();
            }
        }
        return playSessionId;
    }

    void setListening(boolean on) {
        listening = on;
        // 只改音量。开关音轨或改 AudioAttributes 会让 ExoPlayer 拆 RTSP 重拉。
        ui.post(() -> applyAudio(player));
    }

    boolean isListening() {
        return listening;
    }

    void toggleListening() {
        setListening(!listening);
    }

    void setListenLevel(float n) {
        float v = Math.max(0f, Math.min(1f, n));
        if (Math.abs(v - listenLevel) < 0.01f) return;
        listenLevel = v;
        ui.post(() -> applyAudio(player));
    }

    void setAdmPlaying(boolean on) {
        admPlaying = on;
        ui.post(() -> applyAudio(player));
    }

    private void applyAudio(ExoPlayer p) {
        if (p == null) return;
        // 听音走媒体通路+喇叭。通话通路在平板上常进听筒，听球开却没声。
        boolean hear = listenAudio && listening;
        float gain = listenLevel * LISTEN_GAIN;
        if (talking) {
            // 左旋过半听球关掉。0.88 交叉仍留 12%×3.6，球机闭环照样啸。
            float t = talkMix;
            gain = t >= TALK_MUTE_LISTEN ? 0f
                    : gain * (1f - t / TALK_MUTE_LISTEN);
        }
        playGain = hear ? gain : 0f;
        p.setVolume(playGain);
        routeSpeaker(hear);
    }

    private void routeSpeaker(boolean hear) {
        AudioManager am = (AudioManager) view.getContext()
                .getSystemService(Context.AUDIO_SERVICE);
        if (am == null) return;
        if (hear) {
            if (!talking) am.setMode(AudioManager.MODE_NORMAL);
            am.setSpeakerphoneOn(true);
        }
    }

    private void routeComm(boolean comm) {
        AudioManager am = (AudioManager) view.getContext()
                .getSystemService(Context.AUDIO_SERVICE);
        if (am == null) return;
        if (comm) {
            am.setMode(AudioManager.MODE_IN_COMMUNICATION);
            am.setSpeakerphoneOn(true);
        } else {
            am.setSpeakerphoneOn(false);
            am.setMode(AudioManager.MODE_NORMAL);
        }
    }

    private void applyTracks(ExoPlayer p) {
        p.setTrackSelectionParameters(p.getTrackSelectionParameters()
                .buildUpon()
                .setTrackTypeDisabled(C.TRACK_TYPE_AUDIO, !listenAudio)
                .build());
    }

    private static boolean isBallUrl(String u) {
        String s = u == null ? "" : u;
        return s.contains("10.168") || s.contains(":554/11") || s.contains(":554/12");
    }

    String ballHost() {
        return hostOf(url);
    }

    private static String hostOf(String u) {
        if (u == null || u.isEmpty()) return "192.168.10.168";
        int a = u.indexOf("://");
        String s = a >= 0 ? u.substring(a + 3) : u;
        int at = s.indexOf('@');
        if (at >= 0) s = s.substring(at + 1);
        int cut = s.indexOf('/');
        if (cut >= 0) s = s.substring(0, cut);
        int colon = s.indexOf(':');
        if (colon >= 0) s = s.substring(0, colon);
        return s.isEmpty() ? "192.168.10.168" : s;
    }

    void release() {
        ui.removeCallbacks(watch);
        lastBufferedMs = 0;
        aec.reset();
        routeComm(false);
        if (player != null) {
            player.release();
            player = null;
        }
        playing = false;
    }

    private void open() {
        release();
        view.setVisibility(View.VISIBLE);
        try {
            DefaultRenderersFactory rf = new DefaultRenderersFactory(view.getContext()) {
                @Override
                protected AudioSink buildAudioSink(Context context, boolean enableFloatOutput,
                                                   boolean enableAudioTrackPlaybackParams) {
                    return new DefaultAudioSink.Builder(context)
                            .setEnableFloatOutput(enableFloatOutput)
                            .setEnableAudioTrackPlaybackParams(enableAudioTrackPlaybackParams)
                            .setAudioProcessors(new AudioProcessor[] {
                                    new TeeAudioProcessor(farTap)
                            })
                            .build();
                }
            };
            ExoPlayer.Builder eb = new ExoPlayer.Builder(view.getContext())
                    .setRenderersFactory(rf)
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(C.USAGE_MEDIA)
                            .setContentType(C.AUDIO_CONTENT_TYPE_SPEECH)
                            .build(), false);
            int sid = playSessionId();
            ExoPlayer p = eb
                    .setLoadControl(new DefaultLoadControl.Builder()
                            .setBufferDurationsMs(BUFFER_MIN_MS, BUFFER_MAX_MS,
                                    PLAY_AFTER_MS, PLAY_AFTER_REBUFFER_MS)
                            // 直播流按时间判断够不够放，别按字节数 —— 码率一变
                            // 字节阈值对应的时长就跟着变，延迟也跟着飘。
                            .setPrioritizeTimeOverSizeThresholds(true)
                            .build())
                    .build();
            if (sid > 0) p.setAudioSessionId(sid);
            p.addListener(new Player.Listener() {
                @Override
                public void onRenderedFirstFrame() {
                    playing = true;
                    report(true, "");
                }

                @Override
                public void onPlayerError(PlaybackException error) {
                    // 报到界面上去。2.4G 下没画面的原因大多在这句话里：地址不对、
                    // 相机没起来、网卡没绑上，各自的报错完全不同。
                    Log.w(TAG, "player", error);
                    playing = false;
                    report(false, (forceTcp ? "TCP: " : "UDP: ") + brief(error));
                    forceTcp = !forceTcp;
                    scheduleRetry();
                }
            });
            // 布控球音轨可听，HOME 开着才出声。机身 / 2.4G 仍关掉。
            listenAudio = isBallUrl(url) || !bindRadio;
            applyTracks(p);
            applyAudio(p);
            view.setPlayer(p);
            RtspMediaSource.Factory src = new RtspMediaSource.Factory()
                    .setForceUseRtpTcp(forceTcp)
                    .setTimeoutMs(RTSP_TIMEOUT_MS);
            if (bindRadio) {
                src.setSocketFactory(new RadioSocketFactory());
            } else {
                // MESH：播放器走 WiFi / MESH 网卡，直拉球机或机身。
                Network mesh = RadioLink.get().meshNetwork();
                if (mesh != null) src.setSocketFactory(mesh.getSocketFactory());
            }
            p.setMediaSource(src.createMediaSource(MediaItem.fromUri(url)));
            p.prepare();
            p.setPlayWhenReady(true);
            player = p;
            speed = 1.0f;
            ui.removeCallbacks(watch);
            ui.postDelayed(watch, WATCH_MS);
            report(false, "");
        } catch (Throwable t) {
            // 建播放器本身失败（缺库、地址串不合法）也要说出来，不能只留个黑框。
            Log.w(TAG, "open", t);
            report(false, brief(t));
            scheduleRetry();
        }
    }

    /**
     * 把攒下来的那段延迟排掉。
     *
     * 直播流没有「跳到最新」这回事（RTSP 不给 seek），只能靠稍微快放慢慢排；
     * 一次抖动攒得太多就直接重连，重连后 RTSP 从实时点重新 PLAY，等于一步归零。
     */
    private void trimLatency() {
        ExoPlayer p = player;
        if (p == null) return;
        long buffered = p.getTotalBufferedDuration();
        lastBufferedMs = buffered;
        if (playing && buffered > RESYNC_MS) {
            Log.i(TAG, "resync, buffered=" + buffered);
            open();
            return;
        }
        float want = buffered > CATCHUP_MS ? CATCHUP_SPEED : 1.0f;
        if (speed != want) {
            speed = want;
            p.setPlaybackSpeed(want);
        }
        if (playing) report(true, "", buffered);
    }

    private void scheduleRetry() {
        ui.removeCallbacks(retry);
        if (wanted) ui.postDelayed(retry, RETRY_MS);
    }

    private void report(boolean isPlaying, String err) {
        report(isPlaying, err, lastBufferedMs);
    }

    private void report(boolean isPlaying, String err, long bufferedMs) {
        if (listener != null) listener.onVideoState(isPlaying, err, bufferedMs);
    }

    boolean isPlaying() {
        return playing;
    }

    private static String brief(Throwable e) {
        String m = e.getMessage();
        Throwable cause = e.getCause();
        if ((m == null || m.isEmpty()) && cause != null) m = cause.getMessage();
        if (m == null || m.isEmpty()) return e.getClass().getSimpleName();
        return m;
    }

    private final class FarTap implements TeeAudioProcessor.AudioBufferSink {
        private int hz = 48000;
        private int ch = 1;
        private int enc = C.ENCODING_PCM_16BIT;
        private int acc;
        private int accN;
        private final short[] chunk = new short[160];
        private int chunkN;

        @Override
        public void flush(int sampleRateHz, int channelCount, int encoding) {
            hz = sampleRateHz > 0 ? sampleRateHz : 48000;
            ch = channelCount > 0 ? channelCount : 1;
            enc = encoding;
            acc = 0;
            accN = 0;
            chunkN = 0;
        }

        @Override
        public void handleBuffer(ByteBuffer buffer) {
            ByteBuffer b = buffer.order(ByteOrder.LITTLE_ENDIAN);
            int step = Math.max(1, hz / SoftwareAec.HZ);
            float g = playGain / LISTEN_GAIN;
            while (b.remaining() >= bytesPerSample()) {
                int s = readSample(b);
                acc += s;
                accN++;
                if (accN < step) continue;
                int v = acc / accN;
                acc = 0;
                accN = 0;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                chunk[chunkN++] = (short) v;
                if (chunkN == chunk.length) {
                    aec.pushFar(chunk, chunkN, g);
                    chunkN = 0;
                }
            }
        }

        private int bytesPerSample() {
            int w = enc == C.ENCODING_PCM_FLOAT ? 4 : 2;
            return w * ch;
        }

        private int readSample(ByteBuffer b) {
            if (enc == C.ENCODING_PCM_FLOAT) {
                float s = 0f;
                for (int i = 0; i < ch; i++) s += b.getFloat();
                return Math.round((s / ch) * 32767f);
            }
            int s = 0;
            for (int i = 0; i < ch; i++) s += b.getShort();
            return s / ch;
        }
    }

    /**
     * 把播放器的 socket 绑到 2.4G 那张网卡上。
     *
     * 顺序有讲究：先 bind 本地地址（这一步才真正创建出 fd，也把源地址定下来），
     * 再 SO_BINDTODEVICE，最后 connect。反过来做的话 fd 还不存在，设不上选项。
     */
    private static final class RadioSocketFactory extends SocketFactory {

        @Override
        public Socket createSocket() throws IOException {
            return prepare();
        }

        @Override
        public Socket createSocket(String host, int port) throws IOException {
            return connect(prepare(), InetAddress.getByName(host), port);
        }

        // 调用方指定的本地地址一律不理：源地址必须是 2.4G 那张网卡的，
        // 由 prepare() 统一定，否则又会被路由回默认网络。
        @Override
        public Socket createSocket(String host, int port, InetAddress localAddr, int localPort)
                throws IOException {
            return connect(prepare(), InetAddress.getByName(host), port);
        }

        @Override
        public Socket createSocket(InetAddress host, int port) throws IOException {
            return connect(prepare(), host, port);
        }

        @Override
        public Socket createSocket(InetAddress host, int port, InetAddress localAddr, int localPort)
                throws IOException {
            return connect(prepare(), host, port);
        }

        private static Socket connect(Socket s, InetAddress host, int port) throws IOException {
            s.connect(new InetSocketAddress(host, port), CONNECT_TIMEOUT_MS);
            return s;
        }

        private static Socket prepare() throws IOException {
            Socket s = new Socket();
            RadioLink link = RadioLink.get();
            // 系统能看见这张网卡时走公开 API，比反射稳。
            Network net = link.airNetwork();
            if (net != null) {
                try {
                    net.bindSocket(s);
                    return s;
                } catch (Exception e) {
                    Log.w(TAG, "bindSocket", e);
                }
            }
            InetAddress local = link.localAddress();
            if (local != null) {
                try {
                    s.bind(new InetSocketAddress(local, 0));
                } catch (Exception e) {
                    Log.w(TAG, "bind local", e);
                }
            }
            String ifname = link.ifaceName();
            if (!ifname.isEmpty()) {
                try {
                    RadioLink.bindToDevice(s, ifname);
                } catch (Exception e) {
                    // 绑不上就让它按默认网络走：平板同时连着 WiFi 时反而可能通。
                    // 真不通的话上面 connect 会抛出来，界面会显示原因。
                    Log.w(TAG, "bindToDevice", e);
                }
            }
            return s;
        }
    }
}
