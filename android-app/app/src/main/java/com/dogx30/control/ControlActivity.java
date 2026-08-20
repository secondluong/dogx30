package com.dogx30.control;

import android.annotation.SuppressLint;
import android.app.AlertDialog;
import android.os.Build;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.activity.OnBackPressedCallback;
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
    private LinearLayout overlay;
    private TextView overlayMsg;
    private EditText overlayHost;
    private EditText overlayPort;
    private final NativeBridge nativeBridge = new NativeBridge();
    private final G20Rc.Listener rcToJs = this::injectRc;

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
                // file:// 常丢掉 ?shell=app，进包装页就按 App 壳画。
                view.evaluateJavascript(
                        "document.documentElement.classList.add('shell-app');",
                        null);
            }
        });

        // 始终用包装页。去拉网关 HTML 会跑到狗上的旧脚本，顶栏按钮点了也不切 2.4G。
        G20Rc.get().setBackupRadio("radio".equals(GatewayStore.radioPath(this)));
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

        @JavascriptInterface
        public String getGatewayHost() {
            return GatewayStore.host(ControlActivity.this);
        }

        @JavascriptInterface
        public int getGatewayPort() {
            return GatewayStore.port(ControlActivity.this);
        }

        @JavascriptInterface
        public String getRadioPath() {
            return GatewayStore.radioPath(ControlActivity.this);
        }

        @JavascriptInterface
        public void setRadioPath(String path) {
            GatewayStore.saveRadioPath(ControlActivity.this, path);
            G20Rc.get().setBackupRadio(path != null && path.equals("radio"));
        }

        @JavascriptInterface
        public void setMotionDirect(boolean on) {
            G20Rc.get().setWsDown(on);
        }

        @JavascriptInterface
        public void radioCmd(String name) {
            if (name == null) return;
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
            RadioLink.get().setScreenAxes((float) vx, (float) vy, (float) wz);
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

    private void injectRc(G20Rc.Snapshot snap) {
        if (web == null) return;
        web.evaluateJavascript(
                "window.X30Gamepad&&X30Gamepad.onRcChannels&&X30Gamepad.onRcChannels("
                        + snap.toJson() + ")",
                null);
    }

    private void injectKey(KeyEvent event) {
        nativeBridge.rememberKey(event);
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

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
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

    @Override
    protected void onPause() {
        G20Rc.get().removeListener(rcToJs);
        super.onPause();
        // 切后台时页面里的 visibilitychange 会发 release 停车，
        // 这里再显式停一次 JS 定时器，避免系统节流下指令断续送达。
        web.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        G20Rc.get().addListener(rcToJs);
        web.onResume();
    }

    @Override
    protected void onDestroy() {
        if (web != null) {
            web.loadUrl("about:blank");
            web.destroy();
        }
        super.onDestroy();
    }
}
