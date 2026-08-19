package com.dogx30.control;

import android.app.Application;

/** 进程级初始化。RCSDK 官方要求 init 一次，占用的端口也只在这里连一次。 */
public class X30App extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        G20Rc.get().start(this);
    }
}
