package com.dogx30.control;

import android.annotation.SuppressLint;
import android.app.AlertDialog;
import android.graphics.Bitmap;
import android.os.Build;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.TextView;

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
    private TextView overlay;
    private String url;
    private final NativeBridge nativeBridge = new NativeBridge();

    @Override
    @SuppressLint("SetJavaScriptEnabled")
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_control);

        String host = getIntent().getStringExtra(ConnectActivity.KEY_HOST);
        int port = getIntent().getIntExtra(ConnectActivity.KEY_PORT, 8080);
        // shell=app：控制台走平板小屏布局（单背景切换，不要虚拟摇杆）。
        // 网页直接打开 / 时不受影响。
        url = "http://" + host + ":" + port + "/index.html?shell=app";

        // 遥控过程中息屏等于失去控制，必须常亮。
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        goImmersive();

        overlay = findViewById(R.id.overlay);
        web = findViewById(R.id.web);
        web.setFocusable(true);
        web.setFocusableInTouchMode(true);
        web.requestFocus();

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setMediaPlaybackRequiresUserGesture(false);  // 后续视频自动播放
        s.setCacheMode(WebSettings.LOAD_NO_CACHE);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            s.setSafeBrowsingEnabled(false);  // 内网地址，联网校验只会拖慢加载
        }

        // 工业平板上的手柄常常只走系统 KeyEvent，不进浏览器 Gamepad API。
        web.addJavascriptInterface(nativeBridge, "X30Native");

        web.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageStarted(WebView view, String u, Bitmap favicon) {
                showOverlay(getString(R.string.loading));
            }

            @Override
            public void onPageFinished(WebView view, String u) {
                hideOverlay();
                view.requestFocus();
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest request,
                                        WebResourceError error) {
                if (request.isForMainFrame()) {
                    showOverlay(getString(R.string.load_failed, url));
                }
            }
        });

        web.loadUrl(url);

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

    private void showOverlay(String text) {
        overlay.setText(text);
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

    public static class NativeBridge {
        volatile boolean probeOpen;

        @JavascriptInterface
        public void setProbeOpen(boolean open) {
            probeOpen = open;
        }
    }

    private static boolean isProbeKey(int code) {
        return KeyEvent.isGamepadButton(code)
                || (code >= KeyEvent.KEYCODE_DPAD_UP && code <= KeyEvent.KEYCODE_DPAD_CENTER);
    }

    private void injectKey(KeyEvent event) {
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
        if (nativeBridge.probeOpen && isProbeKey(event.getKeyCode())) {
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        int sources = event.getSource();
        if ((sources & InputDevice.SOURCE_CLASS_JOYSTICK) != 0
                && event.getAction() == MotionEvent.ACTION_MOVE) {
            injectAxes(event);
            if (nativeBridge.probeOpen) return true;
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
        super.onPause();
        // 切后台时页面里的 visibilitychange 会发 release 停车，
        // 这里再显式停一次 JS 定时器，避免系统节流下指令断续送达。
        web.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
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
