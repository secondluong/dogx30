package com.dogx30.control;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.GridLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.annotation.NonNull;

/** 连接页完整通道表，遥控页只留名称条。都从 {@link G20Rc} 刷。 */
final class RcHud implements G20Rc.Listener {

    enum Mode { FULL, COMPACT }

    private static final int BG = 0xFF161B22;
    private static final int LINE = 0xFF30363D;
    private static final int TEXT = 0xFFE6EDF3;
    private static final int MUTED = 0xFF8B98A9;
    private static final int ACCENT = 0xFF1F6FEB;
    private static final int GREEN = 0xFF3FB950;
    private static final int WARN = 0xFFD29922;

    private final Mode mode;
    private final TextView status;
    private final TextView event;
    private final LinearLayout names;
    private final GridLayout grid;
    private TextView[] cells = new TextView[0];
    private final TextView[] nameChips = new TextView[G20Rc.NAMES.length];

    RcHud(ViewGroup host, Mode mode) {
        this.mode = mode;
        Context ctx = host.getContext();
        int pad = dp(ctx, 10);

        LinearLayout root = new LinearLayout(ctx);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(BG);
        root.setPadding(pad, pad, pad, pad);
        host.addView(root, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        status = makeText(ctx, 13, MUTED);
        root.addView(status);

        if (mode == Mode.FULL) {
            grid = new GridLayout(ctx);
            grid.setColumnCount(8);
            LinearLayout.LayoutParams gp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            gp.topMargin = dp(ctx, 8);
            root.addView(grid, gp);
        } else {
            grid = null;
        }

        names = new LinearLayout(ctx);
        names.setOrientation(LinearLayout.HORIZONTAL);
        names.setPadding(0, dp(ctx, 8), 0, 0);
        for (int i = 0; i < G20Rc.NAMES.length; i++) {
            final String name = G20Rc.NAMES[i];
            TextView chip = makeText(ctx, 12, MUTED);
            chip.setPadding(dp(ctx, 8), dp(ctx, 6), dp(ctx, 8), dp(ctx, 6));
            chip.setBackgroundColor(0xFF21262D);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            if (i > 0) lp.leftMargin = dp(ctx, 6);
            names.addView(chip, lp);
            chip.setOnClickListener(v -> G20Rc.get().arm(name));
            chip.setOnLongClickListener(v -> {
                G20Rc.get().clearBind(name);
                return true;
            });
            nameChips[i] = chip;
        }
        root.addView(wrapHScroll(ctx, names));

        event = makeText(ctx, 13, TEXT);
        event.setTypeface(Typeface.MONOSPACE);
        event.setPadding(0, dp(ctx, 8), 0, 0);
        root.addView(event);
    }

    void attach() {
        G20Rc.get().addListener(this);
    }

    void detach() {
        G20Rc.get().removeListener(this);
    }

    @Override
    public void onRcUpdate(@NonNull G20Rc.Snapshot snap) {
        status.setText(statusLine(snap));
        event.setText(eventLine(snap));
        if (mode == Mode.FULL) paintGrid(snap);
        paintNames(snap);
    }

    private void paintGrid(G20Rc.Snapshot snap) {
        if (cells.length != snap.ch.length) {
            grid.removeAllViews();
            cells = new TextView[snap.ch.length];
            Context ctx = grid.getContext();
            for (int i = 0; i < snap.ch.length; i++) {
                TextView cell = makeText(ctx, 11, MUTED);
                cell.setTypeface(Typeface.MONOSPACE);
                cell.setGravity(Gravity.CENTER);
                cell.setPadding(dp(ctx, 2), dp(ctx, 6), dp(ctx, 2), dp(ctx, 6));
                GridLayout.LayoutParams lp = new GridLayout.LayoutParams();
                lp.width = 0;
                lp.height = ViewGroup.LayoutParams.WRAP_CONTENT;
                lp.columnSpec = GridLayout.spec(i % 8, 1f);
                lp.rowSpec = GridLayout.spec(i / 8);
                lp.setMargins(dp(ctx, 2), dp(ctx, 2), dp(ctx, 2), dp(ctx, 2));
                grid.addView(cell, lp);
                cells[i] = cell;
            }
        }
        for (int i = 0; i < snap.ch.length; i++) {
            TextView cell = cells[i];
            String bound = "";
            for (java.util.Map.Entry<String, Integer> e : snap.binds.entrySet()) {
                if (e.getValue() == i) {
                    bound = " " + G20Rc.labelOf(e.getKey());
                    break;
                }
            }
            cell.setText("CH" + (i + 1) + "\n" + snap.ch[i] + bound);
            boolean hit = snap.lastCh == i;
            boolean moved = Math.abs(snap.ch[i] - 1500) > 80;
            cell.setTextColor(hit ? Color.WHITE : (moved ? TEXT : MUTED));
            cell.setBackgroundColor(hit ? GREEN : 0xFF21262D);
        }
    }

    private void paintNames(G20Rc.Snapshot snap) {
        java.util.Set<String> down = new java.util.HashSet<>(java.util.Arrays.asList(snap.down));
        for (int i = 0; i < G20Rc.NAMES.length; i++) {
            String name = G20Rc.NAMES[i];
            TextView chip = nameChips[i];
            Integer ch = snap.binds.get(name);
            String label = G20Rc.labelOf(name);
            if (ch != null) label += " CH" + (ch + 1);
            chip.setText(label);
            boolean armed = name.equals(snap.armed);
            boolean on = down.contains(name);
            if (on) {
                chip.setBackgroundColor(GREEN);
                chip.setTextColor(Color.WHITE);
            } else if (armed) {
                chip.setBackgroundColor(WARN);
                chip.setTextColor(0xFF0D1117);
            } else if (ch != null) {
                chip.setBackgroundColor(ACCENT);
                chip.setTextColor(Color.WHITE);
            } else {
                chip.setBackgroundColor(0xFF21262D);
                chip.setTextColor(MUTED);
            }
        }
    }

    private static String statusLine(G20Rc.Snapshot snap) {
        if (!snap.started) return "RCSDK 未启动";
        if (snap.connected) {
            return "App 0.3.0 · 云卓 " + snap.device + " 已接通 · " + snap.ch.length + " 通道";
        }
        return "App 0.3.0 · " + (snap.error.isEmpty() ? "正在连接遥控器…" : snap.error);
    }

    private static String eventLine(G20Rc.Snapshot snap) {
        if (snap.armed.length() > 0) {
            return "请按 " + G20Rc.labelOf(snap.armed) + "。摇杆通道不会被绑上。长按名称可清除。";
        }
        if (snap.lastCh >= 0) {
            String name = snap.lastName.isEmpty() ? "" : (" " + G20Rc.labelOf(snap.lastName));
            return "刚才 CH" + (snap.lastCh + 1) + name
                    + (snap.lastCh < snap.ch.length ? " = " + snap.ch[snap.lastCh] : "")
                    + (snap.error.startsWith("已把") || snap.error.startsWith("刚才") ? "  ·  " + snap.error : "");
        }
        return snap.error.isEmpty()
                ? "点上面的名称，再按对应实体键完成绑定。未知键也可先看哪路 CH 在跳。"
                : snap.error;
    }

    private static TextView makeText(Context ctx, int sp, int color) {
        TextView t = new TextView(ctx);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        return t;
    }

    private static View wrapHScroll(Context ctx, View child) {
        android.widget.HorizontalScrollView sc = new android.widget.HorizontalScrollView(ctx);
        sc.setHorizontalScrollBarEnabled(false);
        sc.addView(child, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return sc;
    }

    private static int dp(Context ctx, int v) {
        return Math.round(v * ctx.getResources().getDisplayMetrics().density);
    }
}
