package org.huxerui.lib.pluginkit.preview;

import android.app.Application;
import android.content.Context;
import org.huxerui.lib.pluginkit.PluginKit;

public final class MainApplication extends Application {
    @Override protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        if (!PluginKit.initializeProcess(this)) System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY);
    }
}
