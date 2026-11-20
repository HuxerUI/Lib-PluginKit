package org.huxerui.lib.pluginkit;

import android.app.Application;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Binder;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import android.os.SystemClock;
import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;

/** Android process bootstrap and package session allocation for PluginKit hosts. */
public final class PluginKit {
    static final String DESCRIPTOR = "org.huxerui.lib.pluginkit.worker.v1";
    static final int READY = IBinder.FIRST_CALL_TRANSACTION;
    static final int OPEN = READY + 1;
    static final int TERMINATE = READY + 2;
    private static final Object LOCK = new Object();
    private static final AtomicLong TOKENS = new AtomicLong();
    private static final Class<?>[] WORKER_SERVICES = {
        WorkerService.Slot0.class, WorkerService.Slot1.class, WorkerService.Slot2.class, WorkerService.Slot3.class
    };
    private static final ArrayList<Slot> SLOTS = new ArrayList<>();
    private static final Map<Long, Slot> SESSIONS = new HashMap<>();
    private static Context context;
    private static boolean workerProcess;

    private PluginKit() {}

    /**
     * Call from Application.attachBaseContext before loading the host native library.
     * Returns true for a PluginKit worker; skip host-only initialization in that process.
     */
    public static boolean initializeProcess(Context owner) {
        synchronized (LOCK) {
            if (context != null) return workerProcess;
            context = owner;
            String process = processName();
            workerProcess = process.startsWith(owner.getPackageName() + ":hpk_worker_");
            System.loadLibrary("hpk_runtime");
            System.loadLibrary("hpk_android");
            nativeInitialize(workerProcess);
            return workerProcess;
        }
    }

    /** Returns the role established by initializeProcess for Application.onCreate guards. */
    public static boolean isWorkerProcess() { return workerProcess; }

    private static String processName() {
        if (Build.VERSION.SDK_INT >= 28) return Application.getProcessName();
        try (FileInputStream file = new FileInputStream("/proc/self/cmdline")) {
            byte[] bytes = new byte[256];
            int count = file.read(bytes);
            int end = 0;
            while (end < count && bytes[end] != 0) ++end;
            return new String(bytes, 0, end, StandardCharsets.UTF_8);
        } catch (Exception error) {
            throw new IllegalStateException("Cannot determine the PluginKit process role", error);
        }
    }

    private static final class Slot implements ServiceConnection {
        final int index;
        final long token = TOKENS.incrementAndGet();
        int packages;
        int pid;
        IBinder control;
        boolean dead;
        boolean retiring;
        boolean bound;
        Slot(int index) { this.index = index; }
        final Binder ready = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                if (code != READY) return super.onTransact(code, data, reply, flags);
                data.enforceInterface(DESCRIPTOR);
                long received = data.readLong();
                IBinder endpoint = data.readStrongBinder();
                int process = data.readInt();
                synchronized (LOCK) {
                    if (received != token || dead) return true;
                    endpoint.linkToDeath(() -> lost(Slot.this), 0);
                    control = endpoint;
                    pid = process;
                    if (retiring) terminate(Slot.this);
                    LOCK.notifyAll();
                }
                return true;
            }
        };
        @Override public void onServiceConnected(ComponentName name, IBinder service) {}
        @Override public void onServiceDisconnected(ComponentName name) { lost(this); }
        @Override public void onBindingDied(ComponentName name) { lost(this); }
        @Override public void onNullBinding(ComponentName name) { lost(this); }
    }

    private static void lost(Slot slot) {
        synchronized (LOCK) {
            slot.dead = true;
            if (slot.bound) {
                slot.bound = false;
                context.unbindService(slot);
            }
            LOCK.notifyAll();
        }
    }

    // Invoked by native transport threads. The bootstrap callback uses Binder threads, independent
    // of the host main thread's ServiceConnection dispatcher, including during a cold synchronous call.
    static long[] open(String path, int moduleFd, long timeout) throws Exception {
        long deadline = SystemClock.elapsedRealtime() + timeout;
        Slot slot;
        long session = TOKENS.incrementAndGet();
        synchronized (LOCK) {
            if (context == null || workerProcess) throw new IllegalStateException("PluginKit host is not initialized");
            if (SLOTS.isEmpty()) {
                for (int i = 0; i < WORKER_SERVICES.length; ++i) SLOTS.add(new Slot(i));
            }
            for (;;) {
                slot = null;
                for (Slot candidate : SLOTS) {
                    if (candidate.retiring && !candidate.dead) continue;
                    int load = candidate.dead ? 0 : candidate.packages;
                    if (slot == null || load < (slot.dead ? 0 : slot.packages)) slot = candidate;
                }
                if (slot != null) break;
                long remaining = deadline - SystemClock.elapsedRealtime();
                if (remaining <= 0) throw new java.util.concurrent.TimeoutException("Previous worker processes have not exited");
                LOCK.wait(remaining);
            }
            if (slot.dead) { slot = new Slot(slot.index); SLOTS.set(slot.index, slot); }
            ++slot.packages;
            SESSIONS.put(session, slot);
        }
        try {
            synchronized (LOCK) {
                if (!slot.bound) {
                    Intent intent = new Intent(context, WORKER_SERVICES[slot.index]);
                    intent.setAction(DESCRIPTOR + "." + slot.token);
                    Bundle extras = new Bundle();
                    extras.putBinder("ready", slot.ready);
                    extras.putLong("token", slot.token);
                    intent.putExtras(extras);
                    slot.bound = context.bindService(intent, slot, Context.BIND_AUTO_CREATE);
                    if (!slot.bound) throw new IllegalStateException("PluginKit Service is not registered");
                }
                while (slot.control == null && !slot.dead) {
                    long remaining = deadline - SystemClock.elapsedRealtime();
                    if (remaining <= 0) throw new java.util.concurrent.TimeoutException("PluginKit Service startup timed out");
                    LOCK.wait(remaining);
                }
                if (slot.dead) throw new IllegalStateException("PluginKit Service exited during startup");
            }
            ParcelFileDescriptor[] sockets = ParcelFileDescriptor.createSocketPair();
            try (ParcelFileDescriptor host = sockets[0]; ParcelFileDescriptor worker = sockets[1];
                 ParcelFileDescriptor module = ParcelFileDescriptor.fromFd(moduleFd)) {
                Parcel data = Parcel.obtain();
                Parcel reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(DESCRIPTOR);
                    data.writeString(path);
                    worker.writeToParcel(data, 0);
                    module.writeToParcel(data, 0);
                    if (!slot.control.transact(OPEN, data, reply, 0)) throw new IllegalStateException("Cannot open worker session");
                    reply.readException();
                    return new long[]{host.detachFd(), slot.pid, session};
                } finally { data.recycle(); reply.recycle(); }
            }
        } catch (Exception error) { release(session, false); throw error; }
    }

    // A successful package Shutdown releases only its channel. Failed exchanges retire the shared
    // process so that plugin code and callbacks cannot outlive revoked invocation grants.
    static void release(long session, boolean graceful) {
        synchronized (LOCK) {
            Slot slot = SESSIONS.remove(session);
            if (slot == null) return;
            --slot.packages;
            if (!graceful || slot.packages == 0) {
                slot.retiring = true;
                if (slot.control != null) terminate(slot);
                // No package channel can be admitted before the control Binder is published.
                // Release failed cold bindings too; a missing onServiceConnected must not consume a slot forever.
                else lost(slot);
            }
        }
    }

    private static void terminate(Slot slot) {
        Parcel data = Parcel.obtain();
        try {
            data.writeInterfaceToken(DESCRIPTOR);
            slot.control.transact(TERMINATE, data, null, IBinder.FLAG_ONEWAY);
        } catch (RemoteException error) { lost(slot); }
        finally { data.recycle(); }
    }

    private static native void nativeInitialize(boolean worker);
    static native void nativeReady();
    static native void nativeOpen(String path, int socket, int module);
}
