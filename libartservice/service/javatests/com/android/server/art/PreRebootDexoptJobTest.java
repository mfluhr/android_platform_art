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

package com.android.server.art;

import static com.android.server.art.PreRebootDexoptJob.JOB_ID;

import static com.google.common.truth.Truth.assertThat;

import static org.mockito.Mockito.any;
import static org.mockito.Mockito.anyBoolean;
import static org.mockito.Mockito.eq;
import static org.mockito.Mockito.isNull;
import static org.mockito.Mockito.lenient;
import static org.mockito.Mockito.never;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.os.CancellationSignal;
import android.os.SystemProperties;
import android.provider.DeviceConfig;

import androidx.test.filters.SmallTest;

import com.android.server.art.model.ArtFlags;
import com.android.server.art.prereboot.PreRebootDriver;
import com.android.server.art.testing.StaticMockitoRule;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.ArgumentCaptor;
import org.mockito.Mock;
import org.mockito.junit.MockitoJUnitRunner;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Future;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;

@SmallTest
@RunWith(MockitoJUnitRunner.StrictStubs.class)
public class PreRebootDexoptJobTest {
    private static final long TIMEOUT_SEC = 10;

    @Rule
    public StaticMockitoRule mockitoRule = new StaticMockitoRule(
            SystemProperties.class, BackgroundDexoptJobService.class, DeviceConfig.class);

    @Mock private PreRebootDexoptJob.Injector mInjector;
    @Mock private JobScheduler mJobScheduler;
    @Mock private PreRebootDriver mPreRebootDriver;
    private PreRebootDexoptJob mPreRebootDexoptJob;

    @Before
    public void setUp() throws Exception {
        // By default, the job is enabled by a build-time flag.
        lenient()
                .when(SystemProperties.getBoolean(eq("pm.dexopt.disable_bg_dexopt"), anyBoolean()))
                .thenReturn(false);
        lenient()
                .when(SystemProperties.getBoolean(eq("dalvik.vm.enable_pr_dexopt"), anyBoolean()))
                .thenReturn(true);
        lenient()
                .when(DeviceConfig.getBoolean(
                        eq(DeviceConfig.NAMESPACE_RUNTIME), eq("enable_pr_dexopt"), anyBoolean()))
                .thenReturn(false);

        lenient().when(mInjector.getJobScheduler()).thenReturn(mJobScheduler);
        lenient().when(mInjector.getPreRebootDriver()).thenReturn(mPreRebootDriver);

        mPreRebootDexoptJob = new PreRebootDexoptJob(mInjector);
        lenient().when(BackgroundDexoptJobService.getJob(JOB_ID)).thenReturn(mPreRebootDexoptJob);
    }

    @Test
    public void testSchedule() throws Exception {
        var captor = ArgumentCaptor.forClass(JobInfo.class);
        when(mJobScheduler.schedule(captor.capture())).thenReturn(JobScheduler.RESULT_SUCCESS);

        assertThat(mPreRebootDexoptJob.schedule()).isEqualTo(ArtFlags.SCHEDULE_SUCCESS);

        JobInfo jobInfo = captor.getValue();
        assertThat(jobInfo.isPeriodic()).isFalse();
        assertThat(jobInfo.isRequireDeviceIdle()).isTrue();
        assertThat(jobInfo.isRequireCharging()).isTrue();
        assertThat(jobInfo.isRequireBatteryNotLow()).isTrue();
    }

    @Test
    public void testScheduleDisabled() {
        when(SystemProperties.getBoolean(eq("pm.dexopt.disable_bg_dexopt"), anyBoolean()))
                .thenReturn(true);

        assertThat(mPreRebootDexoptJob.schedule()).isEqualTo(ArtFlags.SCHEDULE_DISABLED_BY_SYSPROP);

        verify(mJobScheduler, never()).schedule(any());
    }

    @Test
    public void testScheduleNotEnabled() {
        when(SystemProperties.getBoolean(eq("dalvik.vm.enable_pr_dexopt"), anyBoolean()))
                .thenReturn(false);

        assertThat(mPreRebootDexoptJob.schedule()).isEqualTo(ArtFlags.SCHEDULE_DISABLED_BY_SYSPROP);

        verify(mJobScheduler, never()).schedule(any());
    }

    @Test
    public void testScheduleEnabledByPhenotypeFlag() {
        lenient()
                .when(SystemProperties.getBoolean(eq("dalvik.vm.enable_pr_dexopt"), anyBoolean()))
                .thenReturn(false);
        lenient()
                .when(DeviceConfig.getBoolean(
                        eq(DeviceConfig.NAMESPACE_RUNTIME), eq("enable_pr_dexopt"), anyBoolean()))
                .thenReturn(true);
        when(mJobScheduler.schedule(any())).thenReturn(JobScheduler.RESULT_SUCCESS);

        assertThat(mPreRebootDexoptJob.schedule()).isEqualTo(ArtFlags.SCHEDULE_SUCCESS);

        verify(mJobScheduler).schedule(any());
    }

    @Test
    public void testUnschedule() {
        mPreRebootDexoptJob.unschedule();
        verify(mJobScheduler).cancel(JOB_ID);
    }

    @Test
    public void testStart() {
        when(mPreRebootDriver.run(any(), any())).thenReturn(true);

        assertThat(mPreRebootDexoptJob.hasStarted()).isFalse();
        Future<Void> future = mPreRebootDexoptJob.start();
        assertThat(mPreRebootDexoptJob.hasStarted()).isTrue();

        Utils.getFuture(future);
    }

    @Test
    public void testStartAlreadyRunning() {
        Semaphore dexoptDone = new Semaphore(0);
        when(mPreRebootDriver.run(any(), any())).thenAnswer(invocation -> {
            assertThat(dexoptDone.tryAcquire(TIMEOUT_SEC, TimeUnit.SECONDS)).isTrue();
            return true;
        });

        Future<Void> future1 = mPreRebootDexoptJob.start();
        Future<Void> future2 = mPreRebootDexoptJob.start();
        assertThat(future1).isSameInstanceAs(future2);

        dexoptDone.release();
        Utils.getFuture(future1);

        verify(mPreRebootDriver, times(1)).run(any(), any());
    }

    @Test
    public void testStartAnother() {
        when(mPreRebootDriver.run(any(), any())).thenReturn(true);

        Future<Void> future1 = mPreRebootDexoptJob.start();
        Utils.getFuture(future1);
        Future<Void> future2 = mPreRebootDexoptJob.start();
        Utils.getFuture(future2);
        assertThat(future1).isNotSameInstanceAs(future2);
    }

    @Test
    public void testCancel() {
        Semaphore dexoptCancelled = new Semaphore(0);
        Semaphore jobExited = new Semaphore(0);
        when(mPreRebootDriver.run(any(), any())).thenAnswer(invocation -> {
            assertThat(dexoptCancelled.tryAcquire(TIMEOUT_SEC, TimeUnit.SECONDS)).isTrue();
            var cancellationSignal = invocation.<CancellationSignal>getArgument(1);
            assertThat(cancellationSignal.isCanceled()).isTrue();
            jobExited.release();
            return true;
        });

        var unused = mPreRebootDexoptJob.start();
        Future<Void> future = new CompletableFuture().runAsync(
                () -> { mPreRebootDexoptJob.cancel(true /* blocking */); });
        dexoptCancelled.release();
        Utils.getFuture(future);
        // Check that `cancel` is really blocking.
        assertThat(jobExited.tryAcquire()).isTrue();
    }

    @Test
    public void testUpdateOtaSlotOtaThenMainline() {
        mPreRebootDexoptJob.updateOtaSlot("_b");
        mPreRebootDexoptJob.updateOtaSlot(null);

        when(mPreRebootDriver.run(eq("_b"), any())).thenReturn(true);

        Utils.getFuture(mPreRebootDexoptJob.start());
    }

    @Test
    public void testUpdateOtaSlotMainlineThenOta() {
        mPreRebootDexoptJob.updateOtaSlot(null);
        mPreRebootDexoptJob.updateOtaSlot("_a");

        when(mPreRebootDriver.run(eq("_a"), any())).thenReturn(true);

        Utils.getFuture(mPreRebootDexoptJob.start());
    }

    @Test
    public void testUpdateOtaSlotMainlineThenMainline() {
        mPreRebootDexoptJob.updateOtaSlot(null);
        mPreRebootDexoptJob.updateOtaSlot(null);

        when(mPreRebootDriver.run(isNull(), any())).thenReturn(true);

        Utils.getFuture(mPreRebootDexoptJob.start());
    }

    @Test
    public void testUpdateOtaSlotOtaThenOta() {
        mPreRebootDexoptJob.updateOtaSlot("_b");
        mPreRebootDexoptJob.updateOtaSlot("_b");

        when(mPreRebootDriver.run(eq("_b"), any())).thenReturn(true);

        Utils.getFuture(mPreRebootDexoptJob.start());
    }

    @Test(expected = IllegalStateException.class)
    public void testUpdateOtaSlotOtaThenOtaDifferentSlots() {
        mPreRebootDexoptJob.updateOtaSlot("_b");
        mPreRebootDexoptJob.updateOtaSlot("_a");
    }

    @Test(expected = IllegalStateException.class)
    public void testUpdateOtaSlotOtaBogusSlot() {
        mPreRebootDexoptJob.updateOtaSlot("_bogus");
    }
}
