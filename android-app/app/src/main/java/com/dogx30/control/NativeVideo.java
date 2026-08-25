package com.dogx30.control;

import android.net.Network;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.widget.FrameLayout;

import androidx.annotation.Nullable;
import androidx.annotation.OptIn;
import androidx.media3.common.C;
import androidx.media3.common.MediaItem;
import androidx.media3.common.PlaybackException;
import androidx.media3.common.Player;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.exoplayer.DefaultLoadControl;
import androidx.media3.exoplayer.ExoPlayer;
import androidx.media3.exoplayer.rtsp.RtspMediaSource;
import androidx.media3.ui.AspectRatioFrameLayout;
import androidx.media3.ui.PlayerView;

import java.io.IOException;
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
    private boolean playing;
    /**
     * 先试强制 TCP（见类注释）。但个别 RTSP 服务不支持 TCP interleaved，只认 UDP，
     * 那样死磕 TCP 会一路失败到底。所以每次失败后换一种再试：false 那次是
     * 「先 UDP、收不到再自己退 TCP」，覆盖面更广。用的是哪种会写进报错里。
     */
    private boolean forceTcp = true;
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

    /** 反复调用同一个地址是空操作，页面切布局时会调很多次。 */
    void start(String rtspUrl) {
        final String next = rtspUrl == null ? "" : rtspUrl.trim();
        if (next.isEmpty()) return;
        if (wanted && next.equals(url) && player != null) return;
        url = next;
        wanted = true;
        forceTcp = true;
        // 网卡是 RadioLink 找出来并绑上的，它没起来就没有 ifaceName 可绑，
        // RTSP 会被路由到默认网络上去。只看画面不推杆时它可能还没被叫起来。
        RadioLink.get().setEnabled(true);
        open();
    }

    void stop() {
        wanted = false;
        ui.removeCallbacks(retry);
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
        release();
        view.setVisibility(View.GONE);
    }

    void resumeIfWanted() {
        if (wanted && player == null) open();
    }

    void release() {
        ui.removeCallbacks(watch);
        lastBufferedMs = 0;
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
            ExoPlayer p = new ExoPlayer.Builder(view.getContext())
                    .setLoadControl(new DefaultLoadControl.Builder()
                            .setBufferDurationsMs(BUFFER_MIN_MS, BUFFER_MAX_MS,
                                    PLAY_AFTER_MS, PLAY_AFTER_REBUFFER_MS)
                            // 直播流按时间判断够不够放，别按字节数 —— 码率一变
                            // 字节阈值对应的时长就跟着变，延迟也跟着飘。
                            .setPrioritizeTimeOverSizeThresholds(true)
                            .build())
                    .build();
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
            // 关掉音轨。这一路只是拿来看的（网页那侧的 video 一直是 muted，
            // 2.4G 下也没有对讲，对讲在网关那侧）。留着它有两处坏处：画面要跟
            // 音频时钟对齐，AudioTrack 自己那点缓冲就成了延迟下限；而且窄链路上
            // 白占码率。
            p.setTrackSelectionParameters(p.getTrackSelectionParameters()
                    .buildUpon()
                    .setTrackTypeDisabled(C.TRACK_TYPE_AUDIO, true)
                    .build());
            view.setPlayer(p);
            p.setMediaSource(new RtspMediaSource.Factory()
                    .setForceUseRtpTcp(forceTcp)
                    .setSocketFactory(new RadioSocketFactory())
                    .setTimeoutMs(RTSP_TIMEOUT_MS)
                    .createMediaSource(MediaItem.fromUri(url)));
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
