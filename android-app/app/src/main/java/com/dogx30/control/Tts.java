package com.dogx30.control;

import android.content.Context;
import android.media.AudioAttributes;
import android.speech.tts.TextToSpeech;
import android.util.Log;

import java.util.Locale;

/**
 * 按键语音播报的发声端。
 *
 * 为什么非得在原生这侧做：WebView 里没有 Web Speech —— 安卓从来没在 WebView 上实现
 * speechSynthesis。它有时候甚至是**存在**的，但 getVoices() 返回空、speak() 静默不
 * 出声，在电脑浏览器上测完全看不出问题，装到平板上一句都听不到。网页那侧
 * （web/voice.js）念的每一句最后都落到这里。
 *
 * 一律 QUEUE_FLUSH，不排队：遥控台上后一句总比前一句要紧 —— 连按两颗键时人要听的是
 * 后一颗做成了什么。排队播会越积越晚，最后念出来的还是几秒前的动作，比不念更误导。
 */
final class Tts {

    private static final String TAG = "Tts";

    /** 短句念快一点：按键回执要跟得上手，慢半拍不如不念。 */
    private static final float RATE = 1.15f;

    /** 网页问「能不能念」时的三种答复，直接进设置面板那行提示。 */
    static final String INIT = "init";
    static final String OK = "ok";
    static final String NONE = "none";

    private final TextToSpeech tts;
    private volatile String status = INIT;
    /** 引擎起来之前按下的最后一句。开机后第一次按键正好落在这一秒里。 */
    private volatile String pending = "";

    Tts(Context ctx) {
        tts = new TextToSpeech(ctx.getApplicationContext(), this::onInit);
    }

    private void onInit(int code) {
        if (code != TextToSpeech.SUCCESS) {
            Log.w(TAG, "语音引擎起不来：" + code);
            fail();
            return;
        }
        int lang = tts.setLanguage(Locale.CHINA);
        if (lang == TextToSpeech.LANG_MISSING_DATA || lang == TextToSpeech.LANG_NOT_SUPPORTED) {
            // 平板上没装中文语音包。用英文引擎念中文只会出来一串乱码音，不如不念，
            // 设置面板会把原因写出来，让人去系统设置里装一个。
            Log.w(TAG, "没有中文语音：" + lang);
            fail();
            return;
        }
        tts.setSpeechRate(RATE);
        // 跟着媒体音量走：现场调音量按的是音量键，而那颗键默认调的就是媒体音量。
        // 挂到通知或无障碍通道上的话，人把音量拧到底也不知道该去哪里调回来。
        tts.setAudioAttributes(new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                .build());
        status = OK;
        String first = pending;
        pending = "";
        if (!first.isEmpty()) speak(first);
    }

    private void fail() {
        status = NONE;
        pending = "";
    }

    String status() {
        return status;
    }

    /**
     * 念一句。返回 false 表示这句没人念，网页那侧可以自己退回浏览器合成。
     *
     * 由 WebView 的 JS 线程调进来，不在主线程。TextToSpeech 本身就是个跨进程代理，
     * 哪个线程调都行；不特意切回主线程是因为要把「念没念上」同步返回给网页。
     */
    boolean speak(String text) {
        if (text == null) return false;
        String line = text.trim();
        if (line.isEmpty()) return false;
        if (INIT.equals(status)) {
            // 引擎初始化要一秒左右。记下最后一句等它起来补念 —— 丢掉的表现是
            // 「开机后第一次按键不出声」，而那一下往往正是起立。
            pending = line;
            return true;
        }
        if (!OK.equals(status)) return false;
        return tts.speak(line, TextToSpeech.QUEUE_FLUSH, null, "x30") == TextToSpeech.SUCCESS;
    }

    void stop() {
        pending = "";
        try {
            tts.stop();
        } catch (Exception e) {
            Log.w(TAG, "停语音失败", e);
        }
    }

    void shutdown() {
        pending = "";
        try {
            tts.stop();
            tts.shutdown();
        } catch (Exception e) {
            Log.w(TAG, "关语音失败", e);
        }
    }
}
