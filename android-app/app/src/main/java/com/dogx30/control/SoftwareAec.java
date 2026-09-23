package com.dogx30.control;

/**
 * 软件回声消除。听音走 ExoPlayer，麦走 AudioRecord，系统 AEC 对不上参考。
 * 这里把喇叭将要放出的 PCM 当远端，从麦里减掉。
 *
 * 延时搜索 + NLMS 自适应滤波 + 残差压低。双讲时少改系数，避免把人声滤掉。
 */
final class SoftwareAec {

    static final int HZ = 8000;
    private static final int TAP = 1024;
    private static final int FAR_LEN = 8000;
    private static final int MIN_LAG = 480;
    private static final int MAX_LAG = 4000;
    private static final float MU = 0.35f;
    private static final float EPS = 80f;

    private final short[] far = new short[FAR_LEN];
    private final float[] w = new float[TAP];
    private int farW;
    private int delay = 1600;
    private int sinceAlign;
    private int farSilent;

    synchronized void reset() {
        for (int i = 0; i < FAR_LEN; i++) far[i] = 0;
        for (int i = 0; i < TAP; i++) w[i] = 0f;
        farW = 0;
        delay = 1600;
        sinceAlign = 0;
        farSilent = 0;
    }

    synchronized void pushFar(short[] pcm, int n, float playGain) {
        if (pcm == null || n <= 0) return;
        float g = playGain;
        if (g < 0f) g = 0f;
        if (g > 1.3f) g = 1.3f;
        for (int i = 0; i < n; i++) {
            int v = Math.round(pcm[i] * g);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            far[farW] = (short) v;
            farW++;
            if (farW == FAR_LEN) farW = 0;
        }
    }

    synchronized void process(short[] near) {
        if (near == null || near.length == 0) return;
        int farP = 0;
        for (int k = 0; k < TAP; k++) {
            int x = farAt(farW - delay - k);
            farP += (x * x) >> 8;
        }
        if (farP < 40) {
            farSilent++;
            if (farSilent > 80) {
                for (int i = 0; i < TAP; i++) w[i] *= 0.97f;
            }
            return;
        }
        farSilent = 0;

        sinceAlign++;
        if (sinceAlign >= 16) {
            alignDelay(near);
            sinceAlign = 0;
        }

        int n = near.length;
        long nearP = 0;
        long echoP = 0;
        for (int i = 0; i < n; i++) {
            float y = dot(farW - 1 - delay - (n - 1 - i));
            nearP += (long) near[i] * near[i];
            echoP += (long) (y * y);
        }
        boolean doubleTalk = nearP > 8L * (echoP + 1) && nearP > 800L * 800 * n;
        boolean adapt = !doubleTalk && farP > EPS;

        long errP = 0;
        for (int i = 0; i < n; i++) {
            int base = farW - 1 - delay - (n - 1 - i);
            float y = dot(base);
            float d = near[i];
            float e = d - y;
            errP += (long) (e * e);
            if (adapt) {
                float a = MU * e / (farP + EPS);
                if (a > 0.08f) a = 0.08f;
                if (a < -0.08f) a = -0.08f;
                for (int k = 0; k < TAP; k++) {
                    w[k] += a * farAt(base - k);
                }
            }
            int o = Math.round(e);
            if (o > 32767) o = 32767;
            if (o < -32768) o = -32768;
            near[i] = (short) o;
        }

        if (doubleTalk) return;
        if (echoP > nearP / 6 && farP > 80) {
            float g = (float) errP / (nearP + 1f);
            if (g > 1f) g = 1f;
            if (echoP * 2 > nearP) g = Math.min(g, 0.18f);
            if (g < 0.08f) g = 0.08f;
            if (g < 0.95f) {
                for (int i = 0; i < n; i++) {
                    near[i] = (short) Math.round(near[i] * g);
                }
            }
        }
    }

    private float dot(int base) {
        float y = 0f;
        for (int k = 0; k < TAP; k++) {
            y += w[k] * farAt(base - k);
        }
        return y;
    }

    private void alignDelay(short[] near) {
        int take = Math.min(160, near.length);
        if (take < 80) return;
        long t2 = 0;
        for (int i = near.length - take; i < near.length; i++) {
            t2 += (long) near[i] * near[i];
        }
        if (t2 < 200L * 200 * take) return;
        int bestLag = delay;
        float best = 0.12f;
        for (int lag = MIN_LAG; lag <= MAX_LAG; lag += 8) {
            long xf = 0;
            long f2 = 0;
            for (int i = 0; i < take; i++) {
                int f = farAt(farW - 1 - lag - (take - 1 - i));
                int d = near[near.length - take + i];
                xf += (long) d * f;
                f2 += (long) f * f;
            }
            if (f2 < 200L * 200 * take) continue;
            float ncc = (float) (xf * xf) / (float) (t2 * f2);
            if (ncc > best) {
                best = ncc;
                bestLag = lag;
            }
        }
        delay = bestLag;
    }

    private int farAt(int idx) {
        int i = idx % FAR_LEN;
        if (i < 0) i += FAR_LEN;
        return far[i];
    }
}
