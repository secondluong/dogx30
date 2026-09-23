package com.dogx30.control;

import android.content.Context;
import android.media.AudioAttributes;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.speech.tts.TextToSpeech;
import android.speech.tts.UtteranceProgressListener;
import android.util.Log;

import java.io.File;
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
    private final Handler ui = new Handler(Looper.getMainLooper());
    private volatile String status = INIT;
    /** 引擎起来之前按下的最后一句。开机后第一次按键正好落在这一秒里。 */
    private volatile String pending = "";
    private volatile Runnable pendingDone;
    private int utterSeq;

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
            lang = tts.setLanguage(Locale.SIMPLIFIED_CHINESE);
        }
        if (lang == TextToSpeech.LANG_MISSING_DATA || lang == TextToSpeech.LANG_NOT_SUPPORTED) {
            Log.w(TAG, "没有中文语音：" + lang);
            fail();
            return;
        }
        tts.setSpeechRate(RATE);
        // 必须走媒体音量：平板导航音量经常是 0，改导航通道就完全没声。
        // 开麦前先念完再开 AudioRecord，避免媒体通道被麦抢走。
        tts.setAudioAttributes(new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                .build());
        tts.setOnUtteranceProgressListener(new UtteranceProgressListener() {
            @Override public void onStart(String id) {}

            @Override public void onDone(String id) {
                fireDone();
            }

            @Override public void onError(String id) {
                fireDone();
            }
        });
        status = OK;
        String first = pending;
        pending = "";
        Runnable after = pendingDone;
        if (!first.isEmpty()) speakThen(first, after);
        else if (after != null) fireDone();
    }

    private void fireDone() {
        Runnable r = pendingDone;
        pendingDone = null;
        if (r != null) ui.post(r);
    }

    private void fail() {
        status = NONE;
        pending = "";
        fireDone();
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
        return speakThen(text, null);
    }

    /** 念完（或念不了）再跑 after。对讲开必须等这句落地再开麦。 */
    boolean speakThen(String text, Runnable after) {
        pendingDone = after;
        if (text == null) {
            fireDone();
            return false;
        }
        String line = text.trim();
        if (line.isEmpty()) {
            fireDone();
            return false;
        }
        if (INIT.equals(status)) {
            pending = line;
            return true;
        }
        if (!OK.equals(status)) {
            fireDone();
            return false;
        }
        String id = "x30-" + (++utterSeq);
        boolean ok = tts.speak(line, TextToSpeech.QUEUE_FLUSH, null, id)
                == TextToSpeech.SUCCESS;
        if (!ok) fireDone();
        return ok;
    }

    /** 合成到文件，不走喇叭。对讲关麦后把「请您讲」推到球机用。 */
    boolean synthToFile(String text, File file, Runnable after) {
        pendingDone = after;
        if (text == null || file == null || !OK.equals(status)) {
            fireDone();
            return false;
        }
        String line = text.trim();
        if (line.isEmpty()) {
            fireDone();
            return false;
        }
        String id = "x30-s-" + (++utterSeq);
        boolean ok = tts.synthesizeToFile(line, new Bundle(), file, id)
                == TextToSpeech.SUCCESS;
        if (!ok) fireDone();
        return ok;
    }

    void stop() {
        pending = "";
        pendingDone = null;
        try {
            tts.stop();
        } catch (Exception e) {
            Log.w(TAG, "停语音失败", e);
        }
    }

    void shutdown() {
        pending = "";
        pendingDone = null;
        try {
            tts.stop();
            tts.shutdown();
        } catch (Exception e) {
            Log.w(TAG, "关语音失败", e);
        }
    }
}
