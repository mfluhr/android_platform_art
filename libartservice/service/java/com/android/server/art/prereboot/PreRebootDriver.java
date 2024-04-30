/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.server.art.prereboot;

import static com.android.server.art.IDexoptChrootSetup.CHROOT_DIR;

import android.annotation.NonNull;
import android.annotation.Nullable;
import android.content.Context;
import android.os.ArtModuleServiceManager;
import android.os.Build;
import android.os.CancellationSignal;
import android.os.RemoteException;
import android.os.ServiceSpecificException;
import android.util.Log;
import android.util.Slog;

import androidx.annotation.RequiresApi;

import com.android.internal.annotations.VisibleForTesting;
import com.android.server.art.ArtManagerLocal;
import com.android.server.art.ArtModuleServiceInitializer;
import com.android.server.art.GlobalInjector;
import com.android.server.art.IDexoptChrootSetup;
import com.android.server.art.Utils;

import dalvik.system.DelegateLastClassLoader;

/**
 * Drives Pre-reboot Dexopt, through reflection.
 *
 * During Pre-reboot Dexopt, the old version of this code is run.
 *
 * @hide
 */
@RequiresApi(Build.VERSION_CODES.VANILLA_ICE_CREAM)
public class PreRebootDriver {
    private static final String TAG = ArtManagerLocal.TAG;

    @NonNull private final Injector mInjector;

    public PreRebootDriver(@NonNull Context context) {
        this(new Injector(context));
    }

    @VisibleForTesting
    public PreRebootDriver(@NonNull Injector injector) {
        mInjector = injector;
    }

    /**
     * Runs Pre-reboot Dexopt and returns whether it is successful.
     *
     * @param otaSlot The slot that contains the OTA update, "_a" or "_b", or null for a Mainline
     *         update.
     */
    public boolean run(@Nullable String otaSlot, @NonNull CancellationSignal cancellationSignal) {
        try {
            setUp(otaSlot);
            runFromChroot(cancellationSignal);
            return true;
        } catch (RemoteException e) {
            Utils.logArtdException(e);
        } catch (ServiceSpecificException e) {
            Log.e(TAG, "Failed to set up chroot", e);
        } catch (ReflectiveOperationException e) {
            Log.e(TAG, "Failed to run pre-reboot dexopt", e);
        } finally {
            tearDown();
        }
        return false;
    }

    private void setUp(@Nullable String otaSlot) throws RemoteException {
        mInjector.getDexoptChrootSetup().setUp(otaSlot);
    }

    private void tearDown() {
        // In general, the teardown unmounts apexes and partitions, and open files can keep the
        // mounts busy so that they cannot be unmounted. Therefore, two things can prevent the
        // teardown from succeeding: a running Pre-reboot artd process and the new `service-art.jar`
        // opened and mapped by system server. They are managed by the service manager and the
        // runtime respectively. There aren't reliable APIs to kill the former or close the latter,
        // so we have to do them by triggering GC and finalization, with sleep and retry mechanism.
        for (int numRetries = 3; numRetries > 0;) {
            try {
                Runtime.getRuntime().gc();
                Runtime.getRuntime().runFinalization();
                // Wait for the service manager to shut down artd. The shutdown is asynchronous.
                Utils.sleep(5000);
                mInjector.getDexoptChrootSetup().tearDown();
                return;
            } catch (RemoteException e) {
                Utils.logArtdException(e);
            } catch (ServiceSpecificException e) {
                Log.e(TAG, "Failed to tear down chroot", e);
            } catch (IllegalStateException e) {
                // Not expected, but we still want retries in such an extreme case.
                Slog.wtf(TAG, "Unexpected exception", e);
            }

            if (--numRetries > 0) {
                Log.i(TAG, "Retrying....");
                Utils.sleep(30000);
            }
        }
    }

    private void runFromChroot(@NonNull CancellationSignal cancellationSignal)
            throws ReflectiveOperationException {
        String chrootArtDir = CHROOT_DIR + "/apex/com.android.art";
        String dexPath = chrootArtDir + "/javalib/service-art.jar";
        var classLoader =
                new DelegateLastClassLoader(dexPath, this.getClass().getClassLoader() /* parent */);
        Class<?> preRebootManagerClass =
                classLoader.loadClass("com.android.server.art.prereboot.PreRebootManager");
        // Check if the dex file is loaded successfully. Note that the constructor of
        // `DelegateLastClassLoader` does not throw when the load fails.
        if (preRebootManagerClass == PreRebootManager.class) {
            throw new IllegalStateException(String.format("Failed to load %s", dexPath));
        }
        Object preRebootManager = preRebootManagerClass.getConstructor().newInstance();
        preRebootManagerClass
                .getMethod("run", ArtModuleServiceManager.class, Context.class,
                        CancellationSignal.class)
                .invoke(preRebootManager, ArtModuleServiceInitializer.getArtModuleServiceManager(),
                        mInjector.getContext(), cancellationSignal);
    }

    /**
     * Injector pattern for testing purpose.
     *
     * @hide
     */
    @VisibleForTesting
    public static class Injector {
        @NonNull private final Context mContext;

        Injector(@NonNull Context context) {
            mContext = context;
        }

        @NonNull
        public Context getContext() {
            return mContext;
        }

        @NonNull
        public IDexoptChrootSetup getDexoptChrootSetup() {
            return GlobalInjector.getInstance().getDexoptChrootSetup();
        }
    }
}
