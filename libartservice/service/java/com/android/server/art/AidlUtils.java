/*
 * Copyright (C) 2022 The Android Open Source Project
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

import static com.android.server.art.OutputArtifacts.PermissionSettings;
import static com.android.server.art.OutputArtifacts.PermissionSettings.SeContext;
import static com.android.server.art.ProfilePath.PrebuiltProfilePath;
import static com.android.server.art.ProfilePath.PrimaryCurProfilePath;
import static com.android.server.art.ProfilePath.PrimaryRefProfilePath;
import static com.android.server.art.ProfilePath.SecondaryCurProfilePath;
import static com.android.server.art.ProfilePath.SecondaryRefProfilePath;
import static com.android.server.art.ProfilePath.TmpProfilePath;
import static com.android.server.art.ProfilePath.WritableProfilePath;

import android.annotation.NonNull;
import android.annotation.Nullable;

/** @hide */
public final class AidlUtils {
    private AidlUtils() {}

    @NonNull
    private static ArtifactsPath buildArtifactsPath(@NonNull String dexPath, @NonNull String isa,
            boolean isInDalvikCache, boolean isPreReboot) {
        var artifactsPath = new ArtifactsPath();
        artifactsPath.dexPath = dexPath;
        artifactsPath.isa = isa;
        artifactsPath.isInDalvikCache = isInDalvikCache;
        artifactsPath.isPreReboot = isPreReboot;
        return artifactsPath;
    }

    @NonNull
    public static ArtifactsPath buildArtifactsPathAsInput(
            @NonNull String dexPath, @NonNull String isa, boolean isInDalvikCache) {
        // The callers expect artifacts to be used as inputs, so we should always pick the
        // non-Pre-reboot ones.
        return buildArtifactsPath(dexPath, isa, isInDalvikCache, false /* isPreReboot */);
    }

    @NonNull
    public static ArtifactsPath buildArtifactsPathAsInputPreReboot(
            @NonNull String dexPath, @NonNull String isa, boolean isInDalvikCache) {
        // Normally this should not be called, unless the caller really means to use Pre-reboot
        // artifacts as inputs.
        return buildArtifactsPath(dexPath, isa, isInDalvikCache, true /* isPreReboot */);
    }

    @NonNull
    public static FsPermission buildFsPermission(
            int uid, int gid, boolean isOtherReadable, boolean isOtherExecutable) {
        var fsPermission = new FsPermission();
        fsPermission.uid = uid;
        fsPermission.gid = gid;
        fsPermission.isOtherReadable = isOtherReadable;
        fsPermission.isOtherExecutable = isOtherExecutable;
        return fsPermission;
    }

    @NonNull
    public static FsPermission buildFsPermission(int uid, int gid, boolean isOtherReadable) {
        return buildFsPermission(uid, gid, isOtherReadable, false /* isOtherExecutable */);
    }

    @NonNull
    public static DexMetadataPath buildDexMetadataPath(@NonNull String dexPath) {
        var dexMetadataPath = new DexMetadataPath();
        dexMetadataPath.dexPath = dexPath;
        return dexMetadataPath;
    }

    @NonNull
    public static PermissionSettings buildPermissionSettings(@NonNull FsPermission dirFsPermission,
            @NonNull FsPermission fileFsPermission, @Nullable SeContext seContext) {
        var permissionSettings = new PermissionSettings();
        permissionSettings.dirFsPermission = dirFsPermission;
        permissionSettings.fileFsPermission = fileFsPermission;
        permissionSettings.seContext = seContext;
        return permissionSettings;
    }

    @NonNull
    public static OutputArtifacts buildOutputArtifacts(@NonNull String dexPath, @NonNull String isa,
            boolean isInDalvikCache, @NonNull PermissionSettings permissionSettings,
            boolean isPreReboot) {
        var outputArtifacts = new OutputArtifacts();
        outputArtifacts.artifactsPath =
                buildArtifactsPath(dexPath, isa, isInDalvikCache, isPreReboot);
        outputArtifacts.permissionSettings = permissionSettings;
        return outputArtifacts;
    }

    @NonNull
    private static PrimaryRefProfilePath buildPrimaryRefProfilePath(
            @NonNull String packageName, @NonNull String profileName, boolean isPreReboot) {
        var primaryRefProfilePath = new PrimaryRefProfilePath();
        primaryRefProfilePath.packageName = packageName;
        primaryRefProfilePath.profileName = profileName;
        primaryRefProfilePath.isPreReboot = isPreReboot;
        return primaryRefProfilePath;
    }

    @NonNull
    private static SecondaryRefProfilePath buildSecondaryRefProfilePath(
            @NonNull String dexPath, boolean isPreReboot) {
        var secondaryRefProfilePath = new SecondaryRefProfilePath();
        secondaryRefProfilePath.dexPath = dexPath;
        secondaryRefProfilePath.isPreReboot = isPreReboot;
        return secondaryRefProfilePath;
    }

    @NonNull
    public static ProfilePath buildProfilePathForPrimaryRefAsInput(
            @NonNull String packageName, @NonNull String profileName) {
        // The callers expect a profile to be used as an input, so we should always pick the
        // non-Pre-reboot one.
        return ProfilePath.primaryRefProfilePath(
                buildPrimaryRefProfilePath(packageName, profileName, false /* isPreReboot */));
    }

    @NonNull
    public static ProfilePath buildProfilePathForPrebuilt(@NonNull String dexPath) {
        var prebuiltProfilePath = new PrebuiltProfilePath();
        prebuiltProfilePath.dexPath = dexPath;
        return ProfilePath.prebuiltProfilePath(prebuiltProfilePath);
    }

    @NonNull
    public static ProfilePath buildProfilePathForDm(@NonNull String dexPath) {
        return ProfilePath.dexMetadataPath(buildDexMetadataPath(dexPath));
    }

    @NonNull
    public static PrimaryCurProfilePath buildPrimaryCurProfilePath(
            int userId, @NonNull String packageName, @NonNull String profileName) {
        var primaryCurProfilePath = new PrimaryCurProfilePath();
        primaryCurProfilePath.userId = userId;
        primaryCurProfilePath.packageName = packageName;
        primaryCurProfilePath.profileName = profileName;
        return primaryCurProfilePath;
    }

    @NonNull
    public static ProfilePath buildProfilePathForPrimaryCur(
            int userId, @NonNull String packageName, @NonNull String profileName) {
        return ProfilePath.primaryCurProfilePath(
                buildPrimaryCurProfilePath(userId, packageName, profileName));
    }

    @NonNull
    public static ProfilePath buildProfilePathForSecondaryRefAsInput(@NonNull String dexPath) {
        // The callers expect a profile to be used as an input, so we should always pick the
        // non-Pre-reboot one.
        return ProfilePath.secondaryRefProfilePath(
                buildSecondaryRefProfilePath(dexPath, false /* isPreReboot */));
    }

    @NonNull
    public static ProfilePath buildProfilePathForSecondaryCur(@NonNull String dexPath) {
        var secondaryCurProfilePath = new SecondaryCurProfilePath();
        secondaryCurProfilePath.dexPath = dexPath;
        return ProfilePath.secondaryCurProfilePath(secondaryCurProfilePath);
    }

    @NonNull
    private static OutputProfile buildOutputProfile(
            @NonNull WritableProfilePath finalPath, int uid, int gid, boolean isPublic) {
        var outputProfile = new OutputProfile();
        outputProfile.profilePath = new TmpProfilePath();
        outputProfile.profilePath.finalPath = finalPath;
        outputProfile.profilePath.id = ""; // Will be filled by artd.
        outputProfile.profilePath.tmpPath = ""; // Will be filled by artd.
        outputProfile.fsPermission = buildFsPermission(uid, gid, isPublic);
        return outputProfile;
    }

    @NonNull
    public static OutputProfile buildOutputProfileForPrimary(@NonNull String packageName,
            @NonNull String profileName, int uid, int gid, boolean isPublic, boolean isPreReboot) {
        return buildOutputProfile(WritableProfilePath.forPrimary(buildPrimaryRefProfilePath(
                                          packageName, profileName, isPreReboot)),
                uid, gid, isPublic);
    }

    @NonNull
    public static OutputProfile buildOutputProfileForSecondary(
            @NonNull String dexPath, int uid, int gid, boolean isPublic, boolean isPreReboot) {
        return buildOutputProfile(WritableProfilePath.forSecondary(
                                          buildSecondaryRefProfilePath(dexPath, isPreReboot)),
                uid, gid, isPublic);
    }

    @NonNull
    public static SeContext buildSeContext(@NonNull String seInfo, int uid) {
        var seContext = new SeContext();
        seContext.seInfo = seInfo;
        seContext.uid = uid;
        return seContext;
    }

    @NonNull
    public static RuntimeArtifactsPath buildRuntimeArtifactsPath(
            @NonNull String packageName, @NonNull String dexPath, @NonNull String isa) {
        var runtimeArtifactsPath = new RuntimeArtifactsPath();
        runtimeArtifactsPath.packageName = packageName;
        runtimeArtifactsPath.dexPath = dexPath;
        runtimeArtifactsPath.isa = isa;
        return runtimeArtifactsPath;
    }

    @NonNull
    public static SecureDexMetadataWithCompanionPaths buildSecureDexMetadataWithCompanionPaths(
            @NonNull String dexPath, @NonNull String isa, boolean isInDalvikCache) {
        var paths = new SecureDexMetadataWithCompanionPaths();
        paths.dexPath = dexPath;
        paths.isa = isa;
        paths.isInDalvikCache = isInDalvikCache;
        return paths;
    }

    @NonNull
    public static OutputSecureDexMetadataCompanion buildOutputSecureDexMetadataCompanion(
            @NonNull String dexPath, @NonNull String isa, boolean isInDalvikCache,
            @NonNull PermissionSettings permissionSettings) {
        var outputSdc = new OutputSecureDexMetadataCompanion();
        outputSdc.sdcPath = buildSecureDexMetadataWithCompanionPaths(dexPath, isa, isInDalvikCache);
        outputSdc.permissionSettings = permissionSettings;
        return outputSdc;
    }

    @NonNull
    public static WritableProfilePath toWritableProfilePath(@NonNull ProfilePath profile) {
        switch (profile.getTag()) {
            case ProfilePath.primaryRefProfilePath:
                return WritableProfilePath.forPrimary(profile.getPrimaryRefProfilePath());
            case ProfilePath.secondaryRefProfilePath:
                return WritableProfilePath.forSecondary(profile.getSecondaryRefProfilePath());
            default:
                throw new IllegalStateException("ProfilePath tag " + profile.getTag()
                        + " does not represent a writable type");
        }
    }

    @NonNull
    public static String toString(@NonNull PrimaryRefProfilePath profile) {
        return String.format(
                "PrimaryRefProfilePath[packageName = %s, profileName = %s, isPreReboot = %b]",
                profile.packageName, profile.profileName, profile.isPreReboot);
    }

    @NonNull
    public static String toString(@NonNull SecondaryRefProfilePath profile) {
        return String.format("SecondaryRefProfilePath[dexPath = %s, isPreReboot = %b]",
                profile.dexPath, profile.isPreReboot);
    }

    @NonNull
    public static String toString(@NonNull PrebuiltProfilePath profile) {
        return String.format("PrebuiltProfilePath[dexPath = %s]", profile.dexPath);
    }

    @NonNull
    public static String toString(@NonNull DexMetadataPath profile) {
        return String.format("DexMetadataPath[dexPath = %s]", profile.dexPath);
    }

    @NonNull
    public static String toString(@NonNull WritableProfilePath profile) {
        switch (profile.getTag()) {
            case WritableProfilePath.forPrimary:
                return toString(profile.getForPrimary());
            case WritableProfilePath.forSecondary:
                return toString(profile.getForSecondary());
            default:
                throw new IllegalStateException(
                        "Unknown WritableProfilePath tag " + profile.getTag());
        }
    }

    @NonNull
    public static String toString(@NonNull ProfilePath profile) {
        switch (profile.getTag()) {
            case ProfilePath.primaryRefProfilePath:
                return toString(profile.getPrimaryRefProfilePath());
            case ProfilePath.secondaryRefProfilePath:
                return toString(profile.getSecondaryRefProfilePath());
            case ProfilePath.prebuiltProfilePath:
                return toString(profile.getPrebuiltProfilePath());
            case ProfilePath.dexMetadataPath:
                return toString(profile.getDexMetadataPath());
            default:
                throw new UnsupportedOperationException(
                        "Only a subset of profile paths are supported to be converted to string, "
                        + "got " + profile.getTag());
        }
    }

    @NonNull
    public static String toString(@NonNull SecureDexMetadataWithCompanionPaths paths) {
        return String.format(
                "SecureDexMetadataWithCompanionPaths[dexPath = %s, isa = %s, isInDalvikCache = %b]",
                paths.dexPath, paths.isa, paths.isInDalvikCache);
    }
}
