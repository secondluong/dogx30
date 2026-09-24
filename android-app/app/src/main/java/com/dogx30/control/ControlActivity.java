package com.dogx30.control;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.AlertDialog;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.net.ConnectivityManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;

import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.OnBackPressedCallback;
import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;

/**
 * 遥控页。
 *
 * 当前是网关自带控制台的原生壳。这么做不是偷懒：控制台与安卓端共用同一套
 * WebSocket 协议，界面只有一处实现，改一次两端同时生效。后续视频与点云要上
 * 原生渲染时，把对应区域换成 SurfaceView 即可，控制逻辑不用动。
 */
public class ControlActivity extends AppCompatActivity {

    private WebView web;
    private NativeVideo video;
    private NativeWs nativeWs;
    private Tts tts;
    private LinearLayout overlay;
    private TextView overlayMsg;
    private EditText overlayHost;
    private EditText overlayPort;
    private final NativeBridge nativeBridge = new NativeBridge();
    private final G20Rc.Listener rcToJs = this::injectRc;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private Runnable pendingTalkStart;
    private Runnable pendingTalkGo;
    private boolean rcListening;
    private boolean micAsked;
    private boolean pausingForMic;
    /** CH9 起按键的松开值。有的键松开停在 1050，不能再按离 1500 远来判断。 */
    private final int[] btnRest = new int[32];
    private final boolean[] btnDown = new boolean[32];
    private int btnRestTicks;
    private final int[] h16Rest = new int[32];
    private final boolean[] h16Down = new boolean[32];
    private int h16RestTicks;
    private long r1At;
    private long homeAt;
    private long talkKeyAt;
    private boolean talkKnobLive;
    private boolean listenKnobLive;
    private int talkKnobRest;
    private int listenKnobRest;
    private boolean talkKnobOn;
    private boolean listenKnobOn;
    private long rcArmedAt;
    private String pipArmed = "";
    private long pipAt;
    private int pipMode = 2;
    private volatile long rcTickAt;
    /** L1=CH7 / L2=CH8：原生边沿派发，不依赖 WebView 轮询 pollRc。 */
    private int l1Rest;
    private int l2Rest;
    private boolean l1Down;
    private boolean l2Down;
    private int l12PrimeTicks;
    private long l1At;
    private long l2At;

    @Override
    @SuppressLint({"SetJavaScriptEnabled", "SetAllowFileAccess", "SetAllowFileAccessFromFileURLs"})
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_control);

        // 遥控过程中息屏等于失去控制，必须常亮。
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        goImmersive();

        overlay = findViewById(R.id.overlay);
        overlayMsg = findViewById(R.id.overlay_msg);
        overlayHost = findViewById(R.id.overlay_host);
        overlayPort = findViewById(R.id.overlay_port);
        Button overlayRetry = findViewById(R.id.overlay_retry);
        overlayHost.setText(GatewayStore.host(this));
        overlayPort.setText(String.valueOf(GatewayStore.port(this)));
        overlayRetry.setOnClickListener(v -> saveOverlayAndReload());
        web = findViewById(R.id.web);
        web.setFocusable(true);
        web.setFocusableInTouchMode(true);
        web.requestFocus();
        // 原生画面垫在 WebView 底下，网页那层不透明就永远看不到它。
        // 根布局是 #0D1117，与网页 --bg 同色，所以没画面时观感不变。
        web.setBackgroundColor(Color.TRANSPARENT);
        video = new NativeVideo(findViewById(R.id.native_video), this::pushVideoState);
        CameraTalk.get().attachVideo(video);
        nativeWs = new NativeWs(web);
        // 引擎初始化要一秒左右，越早开始越好：开机后第一次按键往往就在这一秒里。
        tts = new Tts(this);
        CameraTalk.get().attachTts(tts);
        CannonLink.get().attach(this);

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setAllowFileAccess(true);
        s.setAllowFileAccessFromFileURLs(true);
        s.setAllowUniversalAccessFromFileURLs(true);
        s.setMediaPlaybackRequiresUserGesture(false);  // 后续视频自动播放
        s.setCacheMode(WebSettings.LOAD_NO_CACHE);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            s.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            s.setSafeBrowsingEnabled(false);  // 内网地址，联网校验只会拖慢加载
        }

        // 工业平板上的手柄常常只走系统 KeyEvent，不进浏览器 Gamepad API。
        web.addJavascriptInterface(nativeBridge, "X30Native");

        web.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView view, String u) {
                hideOverlay();
                view.requestFocus();
                pushRadioPathToWeb();
                syncPipToWeb();
            }
        });

        G20Rc.get().setBackupRadio("radio".equals(GatewayStore.radioPath(this)));
        ensureRcListener();
        loadLocalConsole();

        // 误触返回键会直接退出遥控页，机器狗随即失去控制指令。必须二次确认。
        getOnBackPressedDispatcher().addCallback(this, new OnBackPressedCallback(true) {
            @Override
            public void handleOnBackPressed() {
                confirmExit();
            }
        });
    }

    private void confirmExit() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.exit_title)
                .setMessage(R.string.exit_message)
                .setNegativeButton(R.string.cancel, null)
                .setPositiveButton(R.string.exit_confirm, (d, w) -> finish())
                .show();
    }

    private void saveOverlayAndReload() {
        String host = overlayHost.getText().toString().trim();
        String portText = overlayPort.getText().toString().trim();
        int port;
        try {
            port = Integer.parseInt(portText);
        } catch (NumberFormatException e) {
            overlayMsg.setText(R.string.err_bad_port);
            overlay.setVisibility(View.VISIBLE);
            return;
        }
        if (host.isEmpty()) {
            overlayMsg.setText(R.string.err_no_host);
            overlay.setVisibility(View.VISIBLE);
            return;
        }
        GatewayStore.save(this, host, port);
        reloadGateway();
    }

    void reloadGateway() {
        overlayHost.setText(GatewayStore.host(this));
        overlayPort.setText(String.valueOf(GatewayStore.port(this)));
        loadLocalConsole();
    }

    private void loadLocalConsole() {
        hideOverlay();
        web.loadUrl("file:///android_asset/web/index.html?shell=app");
    }

    private void toggleRadioPath() {
        String next = "radio".equals(GatewayStore.radioPath(this)) ? "mesh" : "radio";
        GatewayStore.saveRadioPath(this, next);
        switchMotionPath(next);
        pushRadioPathToWeb();
    }

    private void switchMotionPath(String path) {
        if (!"radio".equals(path)) {
            // 回 MESH 先立刻停掉 2.4G 心跳，再由网页申请网关控制权。
            G20Rc.get().setBackupRadio(false);
        }
        // 去 2.4G 不在这里立即启动。网页收到网关 yield 确认后会显式调用
        // setRadioControlEnabled(true)，从协议上保证两条心跳不重叠。
    }

    /** 页面脚本可能比 onPageFinished 晚一拍，多试几次才能切到 2.4G。 */
    private void pushRadioPathToWeb() {
        if (web == null) return;
        String path = GatewayStore.radioPath(this);
        web.evaluateJavascript(
                "(function(){"
                        + "document.documentElement.classList.add('shell-app');"
                        + "var n=0;function go(){"
                        + "if(window.app&&app.adoptRadioPath){app.adoptRadioPath('"
                        + path + "');return;}"
                        + "if(++n<25)setTimeout(go,80);}"
                        + "go();})()",
                null);
    }

    private void showOverlay(String text) {
        overlayMsg.setText(text);
        overlayHost.setText(GatewayStore.host(this));
        overlayPort.setText(String.valueOf(GatewayStore.port(this)));
        overlay.setVisibility(View.VISIBLE);
    }

    private void hideOverlay() {
        overlay.setVisibility(View.GONE);
    }

    private void goImmersive() {
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    public class NativeBridge {
        volatile String lastKey = "";
        volatile String lastAxis = "";
        private int keySeq;
        private int axisSeq;

        @JavascriptInterface
        public String pollKey() {
            return lastKey;
        }

        @JavascriptInterface
        public String pollAxis() {
            return lastAxis;
        }

        @JavascriptInterface
        public String pollRc() {
            return G20Rc.get().pollJson();
        }

        /** 原生正在读遥控。网页 HOME/画中画让开，避免开关各一下。 */
        @JavascriptInterface
        public boolean rcButtonsLive() {
            return System.currentTimeMillis() - rcTickAt < 400;
        }

        @JavascriptInterface
        public void wsOpen(String url) {
            runOnUiThread(() -> {
                if (nativeWs != null) nativeWs.open(url);
            });
        }

        @JavascriptInterface
        public void wsSend(String msg) {
            if (nativeWs != null) nativeWs.send(msg);
        }

        @JavascriptInterface
        public void wsClose() {
            runOnUiThread(() -> {
                if (nativeWs != null) nativeWs.close();
            });
        }

        @JavascriptInterface
        public boolean wsAlive() {
            return nativeWs != null && nativeWs.isLive();
        }

        @JavascriptInterface
        public String wsPoll() {
            return nativeWs != null ? nativeWs.poll() : "[]";
        }

        /** 点云二进制帧（Base64 数组）。见 NativeWs.pollBin。 */
        @JavascriptInterface
        public String wsPollBin() {
            return nativeWs != null ? nativeWs.pollBin() : "[]";
        }

        @JavascriptInterface
        public String meshDiag() {
            return RadioLink.get().meshDiag();
        }

        @JavascriptInterface
        public String getGatewayHost() {
            return GatewayStore.host(ControlActivity.this);
        }

        @JavascriptInterface
        public int getGatewayPort() {
            return GatewayStore.port(ControlActivity.this);
        }

        @JavascriptInterface
        public String getAppVersion() {
            try {
                return getPackageManager()
                        .getPackageInfo(getPackageName(), 0).versionName;
            } catch (Exception e) {
                return "";
            }
        }

        @JavascriptInterface
        public String getRadioPath() {
            return GatewayStore.radioPath(ControlActivity.this);
        }

        @JavascriptInterface
        public void setRadioPath(String path) {
            GatewayStore.saveRadioPath(ControlActivity.this, path);
            switchMotionPath(path);
        }

        @JavascriptInterface
        public void setRadioControlEnabled(boolean on) {
            if (on && !"radio".equals(GatewayStore.radioPath(ControlActivity.this))) return;
            G20Rc.get().setBackupRadio(on);
        }

        @JavascriptInterface
        public void toggleRadioPath() {
            runOnUiThread(ControlActivity.this::toggleRadioPath);
        }

        @JavascriptInterface
        public void setMotionDirect(boolean on) {
            G20Rc.get().setWsDown(on);
        }

        @JavascriptInterface
        public void radioCmd(String name) {
            if (name == null) return;
            RadioLink.get().setEnabled(true);
            RadioLink.get().command(name);
        }

        @JavascriptInterface
        public boolean radioStanding() {
            return RadioLink.get().isStanding();
        }

        @JavascriptInterface
        public boolean radioLinkOk() {
            return RadioLink.get().isLinkReady();
        }

        @JavascriptInterface
        public String radioStatus() {
            return RadioLink.get().statusJson();
        }

        @JavascriptInterface
        public void radioVel(double vx, double vy, double wz) {
            RadioLink.get().setScreenAxes((float) vx, (float) vy, (float) wz, 0f);
        }

        @JavascriptInterface
        public void radioVel(double vx, double vy, double wz, double tilt) {
            RadioLink.get().setScreenAxes((float) vx, (float) vy, (float) wz,
                    (float) tilt);
        }

        @JavascriptInterface
        public void radioWalk(String mode) {
            RadioLink.get().adoptWalkMode(mode);
        }

        /** 切到 2.4G 时把 MESH 那侧知道的姿态交接过来，见 RadioLink.adoptPosture。 */
        @JavascriptInterface
        public void radioAdoptPose(boolean standing) {
            RadioLink.get().adoptPosture(standing);
        }

        @JavascriptInterface
        public void radioAdoptMotion(boolean standing, String walk) {
            RadioLink.get().adoptMotion(standing, walk);
        }

        /** 水炮走 2.4G 直连 1 网炮台，不经 MESH。 */
        @JavascriptInterface
        public void radioCannon(double pan, double tilt, String spray, boolean fire) {
            CannonLink.get().set((float) pan, (float) tilt, spray, fire);
        }

        @JavascriptInterface
        public String radioCannonStatus() {
            return CannonLink.get().statusJson();
        }

        /**
         * 按键语音。WebView 里没有 speechSynthesis（见 Tts 的注释），网页那侧念的
         * 每一句都从这里出去。返回 false 时网页会自己想办法。
         */
        @JavascriptInterface
        public boolean speak(String text) {
            Tts t = tts;
            return t != null && t.speak(text);
        }

        @JavascriptInterface
        public void ttsStop() {
            Tts t = tts;
            if (t != null) t.stop();
        }

        /** init / ok / none，见 Tts。设置面板照这个写「为什么不出声」。 */
        @JavascriptInterface
        public String ttsStatus() {
            Tts t = tts;
            return t == null ? Tts.NONE : t.status();
        }

        /** 2.4G 下的机身相机。地址由网页给：现场换相机不该为此重新编包。 */
        @JavascriptInterface
        public void videoStart(String url) {
            runOnUiThread(() -> {
                if (video != null) video.start(url, true);
            });
        }

        /**
         * bindRadio=true 绑 2.4G 网卡（历史路径，网页已不再用）。
         * false 钉 WiFi/MESH：画面与球机载荷不跟运动档位走射频口。
         */
        @JavascriptInterface
        public void videoStartOn(String url, boolean bindRadio) {
            runOnUiThread(() -> {
                if (video != null) video.start(url, bindRadio);
            });
        }

        @JavascriptInterface
        public void videoStop() {
            runOnUiThread(() -> {
                // 网页 hidden 会误报（开麦、画中画重拉都会）。听球只听右旋 / onPause。
                if (video != null) video.stop();
            });
        }

        /** 布控球 anv CGI。球在 10 网，bindRadio=false 钉 WiFi。 */
        @JavascriptInterface
        public void cameraGetFire(String url, boolean bindRadio) {
            CameraCgi.fire(url, bindRadio);
            if (url != null && url.contains("pip_cgi") && url.contains("action=set")) {
                refreshBallVideo();
            }
        }

        /** 网页按钮切画中画后，把平板这条 RTSP 拆掉重拉。 */
        @JavascriptInterface
        public void videoRefresh() {
            refreshBallVideo();
        }

        @JavascriptInterface
        public String cameraGet(String url, boolean bindRadio) {
            return CameraCgi.get(url, bindRadio);
        }

        /** 布控球对讲。球机 SRS 数据通道推 G.711A，不经网关 WHIP。 */
        @JavascriptInterface
        public void talkStart(String host) {
            CameraTalk.get().start(ControlActivity.this, host);
        }

        @JavascriptInterface
        public void talkStop() {
            CameraTalk.get().stop();
        }

        @JavascriptInterface
        public void talkToggle(String host) {
            toggleTalkFromUser(0);
        }

        @JavascriptInterface
        public boolean talkActive() {
            return CameraTalk.get().isOn();
        }

        @JavascriptInterface
        public void listenToggle() {
            toggleListenFromUser();
        }

        @JavascriptInterface
        public boolean listenActive() {
            return video != null && video.isListening();
        }

        /** 画面画在哪块矩形里，单位是设备像素，由网页按 devicePixelRatio 换算后给。 */
        @JavascriptInterface
        public void videoRect(int x, int y, int w, int h) {
            runOnUiThread(() -> {
                if (video != null) video.setRect(x, y, w, h);
            });
        }

        @JavascriptInterface
        public void saveGatewayPrefs(String host, int port) {
            if (host == null) return;
            host = host.trim();
            if (host.isEmpty() || port < 1 || port > 65535) return;
            GatewayStore.save(ControlActivity.this, host, port);
        }

        @JavascriptInterface
        public void setGateway(String host, int port) {
            if (host == null) return;
            host = host.trim();
            if (host.isEmpty() || port < 1 || port > 65535) return;
            GatewayStore.save(ControlActivity.this, host, port);
            runOnUiThread(ControlActivity.this::reloadGateway);
        }

        @JavascriptInterface
        public String devices() {
            StringBuilder sb = new StringBuilder();
            int[] ids = InputDevice.getDeviceIds();
            for (int id : ids) {
                InputDevice d = InputDevice.getDevice(id);
                if (d == null) continue;
                int src = d.getSources();
                if ((src & (InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_JOYSTICK
                        | InputDevice.SOURCE_DPAD)) == 0) {
                    continue;
                }
                if (sb.length() > 0) sb.append(" | ");
                sb.append(d.getName()).append(" #").append(id);
            }
            return sb.toString();
        }

        synchronized void rememberKey(KeyEvent event) {
            keySeq++;
            lastKey = "{\"seq\":" + keySeq
                    + ",\"down\":" + (event.getAction() == KeyEvent.ACTION_DOWN)
                    + ",\"repeat\":" + event.getRepeatCount()
                    + ",\"keyCode\":" + event.getKeyCode()
                    + ",\"scanCode\":" + event.getScanCode()
                    + ",\"deviceId\":" + event.getDeviceId()
                    + ",\"name\":\"" + KeyEvent.keyCodeToString(event.getKeyCode()) + "\"}";
        }

        synchronized void rememberAxis(MotionEvent event) {
            axisSeq++;
            lastAxis = "{\"seq\":" + axisSeq
                    + ",\"lx\":" + event.getAxisValue(MotionEvent.AXIS_X)
                    + ",\"ly\":" + event.getAxisValue(MotionEvent.AXIS_Y)
                    + ",\"rx\":" + event.getAxisValue(MotionEvent.AXIS_Z)
                    + ",\"ry\":" + event.getAxisValue(MotionEvent.AXIS_RZ)
                    + ",\"lt\":" + event.getAxisValue(MotionEvent.AXIS_LTRIGGER)
                    + ",\"rt\":" + event.getAxisValue(MotionEvent.AXIS_RTRIGGER)
                    + ",\"hatx\":" + event.getAxisValue(MotionEvent.AXIS_HAT_X)
                    + ",\"haty\":" + event.getAxisValue(MotionEvent.AXIS_HAT_Y) + "}";
        }
    }

    /** 把播放状态送回网页，占位图上就能写清楚卡在哪，而不是干等。 */
    private void pushVideoState(boolean playing, String err, long bufferedMs) {
        if (web == null) return;
        String safe = err == null ? "" : err.replace("\\", " ").replace("\"", "'")
                .replace("\n", " ").replace("\r", " ");
        web.evaluateJavascript(
                "window.X30DogCam&&X30DogCam.onState({playing:" + playing
                        + ",err:\"" + safe + "\",buf:" + bufferedMs + "})",
                null);
    }

    private void injectRc(G20Rc.Snapshot snap) {
        applyRcButtons(snap);
        if (web == null) return;
        web.evaluateJavascript(
                "window.X30Gamepad&&X30Gamepad.onRcChannels&&X30Gamepad.onRcChannels("
                        + snap.toJson() + ")",
                null);
    }

    /** 左旋 CH11 开麦并调音量，右旋 CH12 听球并调音量。HOME 留给水炮发射。 */
    private void applyRcButtons(G20Rc.Snapshot snap) {
        if (snap == null || !snap.connected) return;
        rcTickAt = System.currentTimeMillis();
        if (rcArmedAt == 0) rcArmedAt = rcTickAt + 800;
        int[] ch = snap.ch;
        if (ch != null && ch.length > 0) {
            scanPwmButtons(ch, btnRest, btnDown, true);
            applyL12(ch);
            applyKnobs(ch);
            applyPipStick(ch);
        }
        if (snap.h16 != null && snap.h16.length > 0) {
            scanPwmButtons(snap.h16, h16Rest, h16Down, false);
        }
    }

    /**
     * L1/L2 必须走原生边沿：网页靠 pollRc 轮询时，安卓桥偶发读不到就整颗键失聪。
     * 相对松开值判定（有的键松开停在 1050），上升沿调网页 cycleWalk / cyclePose。
     */
    private void applyL12(int[] ch) {
        if (ch.length <= 7) return;
        if (l12PrimeTicks < 5) {
            if (ch[6] >= 900 && ch[6] <= 2100) l1Rest = ch[6];
            if (ch[7] >= 900 && ch[7] <= 2100) l2Rest = ch[7];
            l12PrimeTicks++;
            return;
        }
        if (rcTickAt < rcArmedAt) return;
        if (l1Rest == 0 && ch[6] >= 900 && ch[6] <= 2100) l1Rest = ch[6];
        if (l2Rest == 0 && ch[7] >= 900 && ch[7] <= 2100) l2Rest = ch[7];
        boolean d1 = l1Rest != 0 && Math.abs(ch[6] - l1Rest) >= 160;
        boolean d2 = l2Rest != 0 && Math.abs(ch[7] - l2Rest) >= 160;
        long now = rcTickAt;
        if (d1 && !l1Down && now - l1At >= 400) {
            l1At = now;
            web.evaluateJavascript(
                    "try{window.app&&app.cycleWalk&&app.cycleWalk()}catch(e){}",
                    null);
        }
        if (d2 && !l2Down && now - l2At >= 400) {
            l2At = now;
            web.evaluateJavascript(
                    "try{window.app&&app.cyclePose&&app.cyclePose()}catch(e){}",
                    null);
        }
        l1Down = d1;
        l2Down = d2;
    }

    private void scanPwmButtons(int[] ch, int[] rest, boolean[] downAt, boolean primeCh) {
        int last = Math.min(ch.length - 1, rest.length - 1);
        int ticks = primeCh ? btnRestTicks : h16RestTicks;
        if (ticks < 5) {
            for (int i = 8; i <= last; i++) {
                if (skipPwmBtn(i)) continue;
                if (ch[i] >= 900 && ch[i] <= 2100) rest[i] = ch[i];
            }
            if (primeCh) btnRestTicks++;
            else h16RestTicks++;
            return;
        }
        if (rcTickAt < rcArmedAt) return;
        for (int i = 8; i <= last; i++) {
            if (skipPwmBtn(i)) continue;
            int pwm = ch[i];
            if (pwm < 900 || pwm > 2100) continue;
            if (rest[i] == 0) rest[i] = pwm;
            int th = i == 8 ? 120 : 80;
            boolean down = Math.abs(pwm - rest[i]) >= th;
            downAt[i] = down;
        }
    }

    private void applyPipStick(int[] ch) {
        if (ch.length <= 14) return;
        String detent = pipDetent(ch[14]);
        if (video == null || !video.showingBall()) {
            pipArmed = detent;
            return;
        }
        long now = rcTickAt;
        String host = video.ballHost();
        if (pipArmed.isEmpty()) {
            pipArmed = detent;
        } else if (!detent.equals(pipArmed)) {
            if (("next".equals(detent) || "prev".equals(detent))
                    && now - pipAt >= 280) {
                pipAt = now;
                cyclePipNative(host, "next".equals(detent) ? 1 : -1);
            }
            pipArmed = detent;
        }
    }

    /** CH11/CH12 是左右旋钮，CH13–CH16 是小摇杆，都不当按键。 */
    private static boolean skipPwmBtn(int index) {
        return index == 10 || index == 11 || (index >= 12 && index <= 15);
    }

    private static String chSpeak(int chNum) {
        switch (chNum) {
            case 9: return "通道九";
            case 10: return "通道十";
            case 11: return "通道十一";
            case 12: return "通道十二";
            default: return "通道" + chNum;
        }
    }

    private static final int KNOB_MOVE = 70;
    private static final int KNOB_ON = 1240;
    private static final int KNOB_OFF = 1140;

    private void applyKnobs(int[] ch) {
        if (rcTickAt < rcArmedAt) return;
        if (ch.length > 10) applyTalkKnob(ch[10]);
        if (ch.length > 11) applyListenKnob(ch[11]);
    }

    /** 以静止位为中，±350 走满 0..1。短行程旋钮用 1050–1950 会几乎调不动。 */
    private static float knobVol(int pwm, int rest) {
        int mid = rest >= 900 && rest <= 2100 ? rest : 1500;
        float n = (pwm - mid) / 350f + 0.5f;
        if (n < 0f) n = 0f;
        if (n > 1f) n = 1f;
        return n;
    }

    private void applyTalkKnob(int pwm) {
        if (pwm < 900 || pwm > 2100) return;
        if (talkKnobRest == 0) talkKnobRest = pwm;
        float mix = knobVol(pwm, talkKnobRest);
        CameraTalk.get().setOutGain(0.06f + mix * 2.8f);
        if (video != null) video.setTalkMix(mix);
        if (!talkKnobLive) {
            if (Math.abs(pwm - talkKnobRest) < KNOB_MOVE) return;
            talkKnobLive = true;
        }
        boolean want = talkKnobOn ? pwm >= KNOB_OFF : pwm >= KNOB_ON;
        if (want == talkKnobOn) return;
        talkKnobOn = want;
        setTalkFromKnob(want);
    }

    private void applyListenKnob(int pwm) {
        if (pwm < 900 || pwm > 2100) return;
        if (listenKnobRest == 0) listenKnobRest = pwm;
        if (video != null) video.setListenLevel(knobVol(pwm, listenKnobRest));
        if (!listenKnobLive) {
            if (Math.abs(pwm - listenKnobRest) < KNOB_MOVE) return;
            listenKnobLive = true;
        }
        boolean want = listenKnobOn ? pwm >= KNOB_OFF : pwm >= KNOB_ON;
        if (want == listenKnobOn) return;
        listenKnobOn = want;
        setListenFromKnob(want);
    }

    private void setTalkFromKnob(boolean on) {
        talkKeyAt = System.currentTimeMillis();
        String host = video != null ? video.ballHost() : "192.168.10.168";
        if (on) {
            if (CameraTalk.get().isOn() || pendingTalkStart != null) return;
            String line = "对讲开";
            Toast.makeText(this, line, Toast.LENGTH_SHORT).show();
            pendingTalkStart = () -> CameraTalk.get().startFromUser(this, host);
            pendingTalkGo = () -> {
                Runnable r = pendingTalkStart;
                pendingTalkStart = null;
                pendingTalkGo = null;
                if (r != null) r.run();
            };
            if (tts != null) tts.speakThen(line, pendingTalkGo);
            ui.postDelayed(pendingTalkGo, 500);
            return;
        }
        if (pendingTalkGo != null) {
            ui.removeCallbacks(pendingTalkGo);
            pendingTalkGo = null;
        }
        pendingTalkStart = null;
        if (CameraTalk.get().isOn()) CameraTalk.get().stopFromUser();
        announce(0, "对讲关");
    }

    private void setListenFromKnob(boolean on) {
        homeAt = System.currentTimeMillis();
        if (video == null) return;
        video.setListening(on);
        ui.post(() -> announce(0, on ? "听球开" : "听球关"));
    }

    /** 屏幕按钮仍可开关。旋钮走 setTalkFromKnob / setListenFromKnob。 */
    private void toggleListenFromUser() {
        long now = System.currentTimeMillis();
        if (now - homeAt < 350) return;
        homeAt = now;
        if (video == null) return;
        video.toggleListening();
        boolean on = video.isListening();
        ui.post(() -> announce(0, on ? "听球开" : "听球关"));
    }

    /** 屏幕按钮：先念再开麦。麦一抢媒体通道，「对讲开」会被自己掐死。 */
    private void toggleTalkFromUser(int chNum) {
        long now = System.currentTimeMillis();
        if (now - r1At < 350) return;
        r1At = now;
        talkKeyAt = now;
        String host = video != null ? video.ballHost() : "192.168.10.168";
        if (pendingTalkStart != null) return;
        if (CameraTalk.get().isOn()) {
            CameraTalk.get().stopFromUser();
            announce(0, "对讲关");
            return;
        }
        String line = "对讲开";
        Toast.makeText(this, line, Toast.LENGTH_SHORT).show();
        pendingTalkStart = () -> CameraTalk.get().startFromUser(this, host);
        Runnable go = () -> {
            Runnable r = pendingTalkStart;
            pendingTalkStart = null;
            if (r != null) r.run();
        };
        if (tts != null) tts.speakThen(line, go);
        ui.postDelayed(go, 500);
    }

    private void announce(int chNum, String act) {
        String line = chNum > 0 ? chSpeak(chNum) + "，" + act : act;
        if (tts != null) tts.speak(line);
        Toast.makeText(this, line, Toast.LENGTH_SHORT).show();
    }

    private static boolean isListenKey(int kc) {
        return kc == KeyEvent.KEYCODE_HOME
                || kc == KeyEvent.KEYCODE_BUTTON_MODE
                || kc == KeyEvent.KEYCODE_BUTTON_START
                || kc == KeyEvent.KEYCODE_BUTTON_SELECT
                || kc == KeyEvent.KEYCODE_ESCAPE
                || kc == KeyEvent.KEYCODE_MENU;
    }

    private void handleRcKey(KeyEvent e) {
        int kc = e.getKeyCode();
        if (kc == KeyEvent.KEYCODE_BACK || kc == KeyEvent.KEYCODE_VOLUME_UP
                || kc == KeyEvent.KEYCODE_VOLUME_DOWN || kc == KeyEvent.KEYCODE_VOLUME_MUTE
                || kc == KeyEvent.KEYCODE_HOME || kc == KeyEvent.KEYCODE_APP_SWITCH
                || kc == KeyEvent.KEYCODE_POWER || kc == KeyEvent.KEYCODE_ENTER
                || kc == KeyEvent.KEYCODE_DPAD_CENTER) {
            return;
        }
        if (kc == KeyEvent.KEYCODE_BUTTON_R1 || kc == KeyEvent.KEYCODE_BUTTON_5
                || kc == KeyEvent.KEYCODE_BUTTON_R2 || kc == KeyEvent.KEYCODE_BUTTON_6
                || kc == KeyEvent.KEYCODE_BUTTON_7 || kc == KeyEvent.KEYCODE_BUTTON_8) {
            return;
        }
    }

    private static String pipDetent(int pwm) {
        if (pwm < 900 || pwm > 2100) return "mid";
        // 右小行程短，±0.45（1275/1725）现场经常推不到。±0.22 仍高于中位抖动。
        if (pwm <= 1390) return "prev";
        if (pwm >= 1610) return "next";
        return "mid";
    }

    private void cyclePipNative(String host, int step) {
        final String dest = host;
        final int d = step;
        new Thread(() -> {
            int cur = CameraCgi.getPipMode(dest);
            if (cur < 0) cur = pipMode;
            int next = (cur + d + 6) % 6;
            CameraCgi.setPipNow(dest, next);
            int confirmed = CameraCgi.getPipMode(dest);
            if (confirmed >= 0) next = confirmed;
            pipMode = next;
            final int shown = next;
            ui.post(() -> {
                refreshBallVideo();
                String[] names = { "默认", "变焦主图", "热像主图", "左右拼接", "仅变焦", "仅热像" };
                if (tts != null) tts.speak(names[shown]);
                paintPipWeb(shown);
            });
        }, "ptz-pip").start();
    }

    private void refreshBallVideo() {
        if (video != null) video.refreshAfterPip();
    }

    private void syncPipToWeb() {
        final String dest = video != null ? video.ballHost() : "192.168.10.168";
        new Thread(() -> {
            int cur = CameraCgi.getPipMode(dest);
            if (cur < 0) return;
            pipMode = cur;
            ui.post(() -> paintPipWeb(cur));
        }, "ptz-pip-sync").start();
    }

    private void paintPipWeb(int mode) {
        if (web == null) return;
        web.evaluateJavascript(
                "(function(){var n=0;function go(){"
                        + "if(window.X30PtzBall&&X30PtzBall.applyMode){"
                        + "X30PtzBall.applyMode(" + mode + ");return;}"
                        + "if(++n<25)setTimeout(go,80);}go();})()",
                null);
    }

    private void injectKey(KeyEvent event) {
        nativeBridge.rememberKey(event);
        if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
            handleRcKey(event);
        }
        if (web == null) return;
        String name = KeyEvent.keyCodeToString(event.getKeyCode());
        if (name == null) name = "UNKNOWN";
        String js = "window.X30Gamepad&&X30Gamepad.onNativeKey&&X30Gamepad.onNativeKey({"
                + "down:" + (event.getAction() == KeyEvent.ACTION_DOWN) + ","
                + "repeat:" + event.getRepeatCount() + ","
                + "keyCode:" + event.getKeyCode() + ","
                + "scanCode:" + event.getScanCode() + ","
                + "deviceId:" + event.getDeviceId() + ","
                + "name:\"" + name + "\""
                + "})";
        web.evaluateJavascript(js, null);
    }

    private void injectAxes(MotionEvent event) {
        nativeBridge.rememberAxis(event);
        if (web == null) return;
        String js = "window.X30Gamepad&&X30Gamepad.onNativeAxis&&X30Gamepad.onNativeAxis({"
                + "lx:" + event.getAxisValue(MotionEvent.AXIS_X) + ","
                + "ly:" + event.getAxisValue(MotionEvent.AXIS_Y) + ","
                + "rx:" + event.getAxisValue(MotionEvent.AXIS_Z) + ","
                + "ry:" + event.getAxisValue(MotionEvent.AXIS_RZ) + ","
                + "lt:" + event.getAxisValue(MotionEvent.AXIS_LTRIGGER) + ","
                + "rt:" + event.getAxisValue(MotionEvent.AXIS_RTRIGGER) + ","
                + "hatx:" + event.getAxisValue(MotionEvent.AXIS_HAT_X) + ","
                + "haty:" + event.getAxisValue(MotionEvent.AXIS_HAT_Y)
                + "})";
        web.evaluateJavascript(js, null);
    }

    private void injectCannonFire(boolean on) {
        if (web == null) return;
        String js = "window.app&&app.onCannonFire&&app.onCannonFire("
                + (on ? "true" : "false") + ")";
        web.evaluateJavascript(js, null);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (isListenKey(event.getKeyCode())) {
            // HOME 吞掉以免退到桌面。水炮模式按住开阀，侦检模式仍无动作。
            // 听球改由右旋 CH12。
            if (event.getKeyCode() == KeyEvent.KEYCODE_HOME) {
                if (event.getAction() == KeyEvent.ACTION_DOWN
                        && event.getRepeatCount() == 0) {
                    injectCannonFire(true);
                } else if (event.getAction() == KeyEvent.ACTION_UP) {
                    injectCannonFire(false);
                }
            }
            return true;
        }
        injectKey(event);
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        int sources = event.getSource();
        if ((sources & InputDevice.SOURCE_CLASS_JOYSTICK) != 0
                && event.getAction() == MotionEvent.ACTION_MOVE) {
            injectAxes(event);
        }
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) goImmersive();
    }

    private void ensureRcListener() {
        if (rcListening) return;
        G20Rc.get().addListener(rcToJs);
        rcListening = true;
    }

    @Override
    protected void onPause() {
        super.onPause();
        // 开麦、播报、麦标都会走 onPause。这里停对讲或停 TTS，就是现场
        // 「对讲开只有字、麦标闪一下」。真切后台留给 onStop。
        if (pausingForMic || CameraTalk.get().isOn() || pendingTalkStart != null
                || System.currentTimeMillis() - talkKeyAt < 4000
                || System.currentTimeMillis() - homeAt < 4000) {
            return;
        }
        web.onPause();
        if (video != null) video.pauseForBackground();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions,
                                           @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        CameraTalk.get().onPermission(this, requestCode, grantResults);
    }

    @Override
    protected void onResume() {
        super.onResume();
        pausingForMic = false;
        ensureRcListener();
        web.onResume();
        if (CameraTalk.get().isOn() || pendingTalkStart != null) {
            return;
        }
        // 上一包对讲可能把进程钉在 MESH 上，画中画 CGI 会全部打不出去。
        if (Build.VERSION.SDK_INT >= 23) {
            try {
                ConnectivityManager cm = (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
                if (cm != null) cm.bindProcessToNetwork(null);
            } catch (Exception ignored) {}
        }
        if (video != null) video.resumeIfWanted();
        askMicOnce();
    }

    @Override
    protected void onStop() {
        super.onStop();
        injectCannonFire(false);
        if (pausingForMic || CameraTalk.get().isOn() || pendingTalkStart != null
                || System.currentTimeMillis() - homeAt < 4000) return;
        if (video != null) video.pauseForBackground();
    }

    /** 进页就要麦权，左旋开麦才不会再弹麦克风框。 */
    private void askMicOnce() {
        if (micAsked) return;
        micAsked = true;
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
                == PackageManager.PERMISSION_GRANTED) {
            return;
        }
        pausingForMic = true;
        ActivityCompat.requestPermissions(this,
                new String[]{Manifest.permission.RECORD_AUDIO}, CameraTalk.REQ_MIC);
    }

    @Override
    protected void onDestroy() {
        if (pendingTalkGo != null) {
            ui.removeCallbacks(pendingTalkGo);
            pendingTalkGo = null;
        }
        pendingTalkStart = null;
        if (rcListening) {
            G20Rc.get().removeListener(rcToJs);
            rcListening = false;
        }
        if (nativeWs != null) nativeWs.close();
        if (video != null) video.stop();
        CameraTalk.get().release();
        // 不 shutdown 的话引擎连接会一直挂着，下次进来再 new 一个就是泄漏。
        if (tts != null) tts.shutdown();
        if (web != null) {
            web.loadUrl("about:blank");
            web.destroy();
        }
        super.onDestroy();
    }
}
