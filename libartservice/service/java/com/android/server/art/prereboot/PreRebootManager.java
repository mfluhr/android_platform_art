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

import static com.android.server.art.model.DexoptResult.PackageDexoptResult;
import static com.android.server.art.proto.PreRebootStats.Status;

import android.annotation.NonNull;
import android.content.Context;
import android.os.ArtModuleServiceManager;
import android.os.Build;
import android.os.CancellationSignal;

import androidx.annotation.RequiresApi;

import com.android.server.LocalManagerRegistry;
import com.android.server.art.ArtManagerLocal;
import com.android.server.art.ArtdRefCache;
import com.android.server.art.AsLog;
import com.android.server.art.ReasonMapping;
import com.android.server.art.Utils;
import com.android.server.art.model.ArtFlags;
import com.android.server.art.model.DexoptResult;
import com.android.server.art.model.OperationProgress;
import com.android.server.pm.PackageManagerLocal;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;

/**
 * Implementation of {@link PreRebootManagerInterface}.
 *
 * DO NOT add a constructor with parameters! There can't be stability guarantees on constructors as
 * they can't be checked against the interface.
 *
 * During Pre-reboot Dexopt, the new version of this code is run.
 *
 * @hide
 */
@RequiresApi(Build.VERSION_CODES.VANILLA_ICE_CREAM)
public class PreRebootManager implements PreRebootManagerInterface {
    public void run(@NonNull ArtModuleServiceManager artModuleServiceManager,
            @NonNull Context context, @NonNull CancellationSignal cancellationSignal) {
        ExecutorService callbackExecutor = Executors.newSingleThreadExecutor();
        try {
            PreRebootGlobalInjector.init(artModuleServiceManager, context);
            ArtManagerLocal artManagerLocal = new ArtManagerLocal(context);
            PackageManagerLocal packageManagerLocal = Objects.requireNonNull(
                    LocalManagerRegistry.getManager(PackageManagerLocal.class));

            var statsReporter = new PreRebootStatsReporter();
            statsReporter.load();

            // Contains three values: skipped, performed, failed.
            List<Integer> values = new ArrayList(List.of(0, 0, 0));

            // Record every progress change right away, in case the job is interrupted by a reboot.
            Consumer<OperationProgress> progressCallback = progress -> {
                PackageDexoptResult result = progress.getLastPackageDexoptResult();
                if (result == null) {
                    return;
                }
                switch (result.getStatus()) {
                    case DexoptResult.DEXOPT_SKIPPED:
                        values.set(0, values.get(0) + 1);
                        break;
                    case DexoptResult.DEXOPT_PERFORMED:
                        values.set(1, values.get(1) + 1);
                        break;
                    case DexoptResult.DEXOPT_FAILED:
                        values.set(2, values.get(2) + 1);
                        break;
                    case DexoptResult.DEXOPT_CANCELLED:
                        break;
                    default:
                        throw new IllegalStateException("Unknown status: " + result.getStatus());
                }

                statsReporter.recordProgress(
                        values.get(0), values.get(1), values.get(2), progress.getTotal());
            };

            // Record `STATUS_FINISHED` even if the result is `DEXOPT_FAILED`. This is because
            // `DEXOPT_FAILED` means dexopt failed for some packages, while the job is considered
            // successful overall.
            artManagerLocal.addDexoptDoneCallback(false /* onlyIncludeUpdates */, callbackExecutor,
                    (result)
                            -> statsReporter.recordJobEnded(
                                    result.getFinalStatus() == DexoptResult.DEXOPT_CANCELLED
                                            ? Status.STATUS_CANCELLED
                                            : Status.STATUS_FINISHED));

            try (var snapshot = packageManagerLocal.withFilteredSnapshot()) {
                artManagerLocal.dexoptPackages(snapshot, ReasonMapping.REASON_PRE_REBOOT_DEXOPT,
                        cancellationSignal, callbackExecutor,
                        Map.of(ArtFlags.PASS_MAIN, progressCallback));
            }
        } finally {
            ArtdRefCache.getInstance().reset();
            callbackExecutor.shutdown();
            try {
                // Make sure we have no running threads when we tear down.
                callbackExecutor.awaitTermination(Integer.MAX_VALUE, TimeUnit.SECONDS);
            } catch (InterruptedException e) {
                AsLog.wtf("Interrupted", e);
            }
        }
    }
}
