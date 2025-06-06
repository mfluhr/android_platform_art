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

import java.lang.invoke.MethodHandle;

import annotations.ConstantMethodHandle;

public class InvokeStatic {
    private int instanceField;

    private static void unreachable() {
        throw new AssertionError("unreachable!");
    }

    public void method() {}

    @ConstantMethodHandle(
        kind = ConstantMethodHandle.INVOKE_STATIC,
        owner = "InvokeStatic",
        fieldOrMethodName = "method",
        descriptor = "()V")
    private static MethodHandle forInstanceMethod() {
        unreachable();
        return null;
    }

    @ConstantMethodHandle(
        kind = ConstantMethodHandle.INVOKE_STATIC,
        owner = "java/util/List",
        fieldOrMethodName = "size",
        descriptor = "()I")
    private static MethodHandle forInterfaceMethod() {
        unreachable();
        return null;
    }

    @ConstantMethodHandle(
        kind = ConstantMethodHandle.INVOKE_STATIC,
        owner = "Main",
        fieldOrMethodName = "instanceMethod",
        descriptor = "()V")
    private static MethodHandle inaccessibleInstanceMethod() {
        unreachable();
        return null;
    }

    @ConstantMethodHandle(
        kind = ConstantMethodHandle.INVOKE_STATIC,
        owner = "Main",
        fieldOrMethodName = "staticMethod",
        descriptor = "()V")
    private static MethodHandle inaccessibleStaticMethod() {
        unreachable();
        return null;
    }

    public static void runTests() {
        try {
            forInstanceMethod();
            unreachable();
        } catch (IncompatibleClassChangeError expected) {}

        try {
            forInterfaceMethod();
            unreachable();
        } catch (IncompatibleClassChangeError expected) {}

        try {
            InvokeStaticForConstructor.runTests();
            unreachable();
        } catch (IncompatibleClassChangeError | ClassFormatError expected) {}

        try {
            InvokeStaticForClassInitializer.runTests();
            unreachable();
        } catch (IncompatibleClassChangeError | ClassFormatError expected) {}

        try {
            inaccessibleInstanceMethod();
            unreachable();
        } catch (IncompatibleClassChangeError expected) {}

        try {
            inaccessibleStaticMethod();
            unreachable();
        } catch (IncompatibleClassChangeError expected) {}
    }

}
