package com.dogx30.control;

import android.content.Intent;
import android.content.SharedPreferences;
import android.os.AsyncTask;
import android.os.Bundle;
import android.text.TextUtils;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;

import java.net.HttpURLConnection;
import java.net.URL;

/** 连接页：填写 RK3588 地址，并现场看 G20 通道是否在跳。 */
public class ConnectActivity extends AppCompatActivity {

    static final String PREFS = "x30";
    static final String KEY_HOST = "host";
    static final String KEY_PORT = "port";

    private EditText hostInput;
    private EditText portInput;
    private Button connectButton;
    private TextView hint;
    private RcHud rcHud;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_connect);

        hostInput = findViewById(R.id.input_host);
        portInput = findViewById(R.id.input_port);
        connectButton = findViewById(R.id.btn_connect);
        hint = findViewById(R.id.hint);

        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        hostInput.setText(prefs.getString(KEY_HOST, "192.168.1.120"));
        portInput.setText(String.valueOf(prefs.getInt(KEY_PORT, 8080)));

        connectButton.setOnClickListener(v -> attemptConnect());

        FrameLayout host = findViewById(R.id.rc_host);
        rcHud = new RcHud(host, RcHud.Mode.FULL);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (rcHud != null) rcHud.attach();
    }

    @Override
    protected void onPause() {
        if (rcHud != null) rcHud.detach();
        super.onPause();
    }

    private void attemptConnect() {
        String host = hostInput.getText().toString().trim();
        String portText = portInput.getText().toString().trim();
        if (TextUtils.isEmpty(host)) {
            hint.setText(R.string.err_no_host);
            return;
        }
        int port;
        try {
            port = Integer.parseInt(portText);
        } catch (NumberFormatException e) {
            hint.setText(R.string.err_bad_port);
            return;
        }

        setBusy(true);
        new ProbeTask(this, host, port).execute();
    }

    private void setBusy(boolean busy) {
        connectButton.setEnabled(!busy);
        connectButton.setText(busy ? R.string.connecting : R.string.connect);
        hint.setText(busy ? getString(R.string.probing) : "");
    }

    void onProbeResult(String host, int port, String error) {
        setBusy(false);
        if (error != null) {
            hint.setText(getString(R.string.err_unreachable, error));
            return;
        }
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                .putString(KEY_HOST, host)
                .putInt(KEY_PORT, port)
                .apply();

        Intent intent = new Intent(this, ControlActivity.class);
        intent.putExtra(KEY_HOST, host);
        intent.putExtra(KEY_PORT, port);
        startActivity(intent);
    }

    private static class ProbeTask extends AsyncTask<Void, Void, String> {
        private final ConnectActivity activity;
        private final String host;
        private final int port;

        ProbeTask(ConnectActivity activity, String host, int port) {
            this.activity = activity;
            this.host = host;
            this.port = port;
        }

        @Override
        protected String doInBackground(Void... args) {
            HttpURLConnection conn = null;
            try {
                URL url = new URL("http://" + host + ":" + port + "/");
                conn = (HttpURLConnection) url.openConnection();
                conn.setConnectTimeout(3000);
                conn.setReadTimeout(3000);
                conn.setRequestMethod("GET");
                int code = conn.getResponseCode();
                if (code != 200) {
                    return "HTTP " + code;
                }
                return null;
            } catch (Exception e) {
                return e.getClass().getSimpleName();
            } finally {
                if (conn != null) conn.disconnect();
            }
        }

        @Override
        protected void onPostExecute(String error) {
            activity.onProbeResult(host, port, error);
        }
    }
}
