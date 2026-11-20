package org.huxerui.lib.pluginkit.preview;

import org.huxerui.HuxerUIActivity;
import android.os.Bundle;
import java.io.File;
import java.io.IOException;

public final class MainActivity extends HuxerUIActivity {
    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        String commands = getIntent().getStringExtra("commands");
        if (BuildConfig.DEBUG && commands != null && savedInstanceState == null) {
            try {
                String root = new File(getFilesDir(), "pluginkit-csv").getCanonicalPath() + File.separator;
                String script = new File(commands).getCanonicalPath();
                if (!script.startsWith(root)) throw new IllegalArgumentException("CSV commands must be in the private verification directory");
                nativeRun(script);
            } catch (IOException error) { throw new IllegalArgumentException("Cannot resolve CSV commands", error); }
        }
    }
    private static native void nativeRun(String commands);
}
