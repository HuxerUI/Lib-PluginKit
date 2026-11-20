package org.huxerui.lib.pluginkit;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.RemoteException;
import org.huxerui.HuxerUIApplication;

/** Private bound worker with one windowless HuxerUI Runtime and independent package channels. */
public abstract class WorkerService extends Service {
    // Android requires a distinct manifest component for each worker process.
    public static final class Slot0 extends WorkerService {}
    public static final class Slot1 extends WorkerService {}
    public static final class Slot2 extends WorkerService {}
    public static final class Slot3 extends WorkerService {}

    private final Binder control = new Binder() {
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code != PluginKit.OPEN && code != PluginKit.TERMINATE) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(PluginKit.DESCRIPTOR);
            if (Binder.getCallingUid() != Process.myUid()) throw new SecurityException("Foreign PluginKit caller");
            if (code == PluginKit.TERMINATE) { Process.killProcess(Process.myPid()); return true; }
            String path = data.readString();
            try (ParcelFileDescriptor socket = ParcelFileDescriptor.CREATOR.createFromParcel(data);
                 ParcelFileDescriptor module = ParcelFileDescriptor.CREATOR.createFromParcel(data)) {
                PluginKit.nativeOpen(path, socket.detachFd(), module.detachFd());
                reply.writeNoException();
                return true;
            } catch (Exception error) { throw new IllegalStateException("Cannot open plugin channel", error); }
        }
    };

    @Override public void onCreate() {
        super.onCreate();
        if (!PluginKit.initializeProcess(getApplication())) throw new IllegalStateException("Worker must have a private process");
        HuxerUIApplication.initialize(this);
        PluginKit.nativeReady();
    }

    @Override public IBinder onBind(Intent intent) {
        IBinder ready = intent.getExtras().getBinder("ready");
        try {
            // A worker belongs to this host binding; never survive the owning host process.
            ready.linkToDeath(() -> Process.killProcess(Process.myPid()), 0);
            Parcel data = Parcel.obtain();
            try {
                data.writeInterfaceToken(PluginKit.DESCRIPTOR);
                data.writeLong(intent.getLongExtra("token", 0));
                data.writeStrongBinder(control);
                data.writeInt(Process.myPid());
                ready.transact(PluginKit.READY, data, null, IBinder.FLAG_ONEWAY);
            } finally { data.recycle(); }
        } catch (RemoteException error) { Process.killProcess(Process.myPid()); }
        return control;
    }
}
