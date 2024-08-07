/*
 * Copyright (C) 2016 The Android Open Source Project
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

interface Itf {
  public Class<?> sameInvokeInterface();
  public Class<?> sameInvokeInterface2();
  public Class<?> sameInvokeInterface3();
}

public class Main implements Itf {
  public static void assertEquals(Object expected, Object actual) {
    if (expected != actual) {
      throw new Error("Expected " + expected  + ", got " + actual);
    }
  }

  public static void assertEquals(int expected, int actual) {
    if (expected != actual) {
      throw new Error("Expected " + expected  + ", got " + actual);
    }
  }

  public static void main(String[] args) throws Exception {
    System.loadLibrary(args[0]);
    Main[] mains = new Main[3];
    Itf[] itfs = new Itf[3];
    itfs[0] = mains[0] = new Main();
    itfs[1] = mains[1] = new Subclass();
    itfs[2] = mains[2] = new OtherSubclass();

    // Compile methods baseline to start filling inline caches.
    ensureJitBaselineCompiled(Main.class, "$noinline$testInvokeVirtual");
    ensureJitBaselineCompiled(Main.class, "$noinline$testInvokeInterface");
    ensureJitBaselineCompiled(Main.class, "$noinline$testInvokeInterface2");
    ensureJitBaselineCompiled(Main.class, "$noinline$testInlineToSameTarget");

    // Make $noinline$testInvokeVirtual and $noinline$testInvokeInterface hot to get them jitted.
    // We pass Main and Subclass to get polymorphic inlining based on calling
    // the same method.
    for (int i = 0; i < 0x30000; ++i) {
      $noinline$testInvokeVirtual(mains[0]);
      $noinline$testInvokeVirtual(mains[1]);
      $noinline$testInvokeInterface(itfs[0]);
      $noinline$testInvokeInterface(itfs[1]);
      $noinline$testInvokeInterface2(itfs[0]);
      $noinline$testInvokeInterface2(itfs[1]);
      $noinline$testInlineToSameTarget(mains[0]);
      $noinline$testInlineToSameTarget(mains[1]);
    }

    ensureJittedAndPolymorphicInline("$noinline$testInvokeVirtual");
    ensureJittedAndPolymorphicInline("$noinline$testInvokeInterface");
    ensureJittedAndPolymorphicInline("$noinline$testInvokeInterface2");
    ensureJittedAndPolymorphicInline("$noinline$testInlineToSameTarget");

    // At this point, the JIT should have compiled both methods, and inline
    // sameInvokeVirtual and sameInvokeInterface.
    assertEquals(Main.class, $noinline$testInvokeVirtual(mains[0]));
    assertEquals(Main.class, $noinline$testInvokeVirtual(mains[1]));

    assertEquals(Itf.class, $noinline$testInvokeInterface(itfs[0]));
    assertEquals(Itf.class, $noinline$testInvokeInterface(itfs[1]));

    assertEquals(Itf.class, $noinline$testInvokeInterface2(itfs[0]));
    assertEquals(Itf.class, $noinline$testInvokeInterface2(itfs[1]));

    // This will trigger a deoptimization of the compiled code.
    assertEquals(OtherSubclass.class, $noinline$testInvokeVirtual(mains[2]));
    assertEquals(OtherSubclass.class, $noinline$testInvokeInterface(itfs[2]));
    assertEquals(null, $noinline$testInvokeInterface2(itfs[2]));

    // Run this once to make sure we execute the JITted code.
    $noinline$testInlineToSameTarget(mains[0]);
    assertEquals(0x60000 + 1, counter);
  }

  public Class<?> sameInvokeVirtual() {
    field.getClass(); // null check to ensure we get an inlined frame in the CodeInfo.
    return Main.class;
  }

  public Class<?> sameInvokeInterface() {
    field.getClass(); // null check to ensure we get an inlined frame in the CodeInfo.
    return Itf.class;
  }

  public Class<?> sameInvokeInterface2() {
    field.getClass(); // null check to ensure we get an inlined frame in the CodeInfo.
    return Itf.class;
  }

  public Class<?> sameInvokeInterface3() {
    field.getClass(); // null check to ensure we get an inlined frame in the CodeInfo.
    return Itf.class;
  }

  public static Class<?> $noinline$testInvokeInterface(Itf i) {
    return i.sameInvokeInterface();
  }

  public static Class<?> $noinline$testInvokeInterface2(Itf i) {
    // Make three interface calls that will do a ClassTableGet to ensure bogus code
    // generation of ClassTableGet will crash.
    i.sameInvokeInterface();
    i.sameInvokeInterface2();
    return i.sameInvokeInterface3();
  }

  public static Class<?> $noinline$testInvokeVirtual(Main m) {
    return m.sameInvokeVirtual();
  }

  public static void $noinline$testInlineToSameTarget(Main m) {
    m.increment();
  }

  public Object field = new Object();

  public static void ensureJittedAndPolymorphicInline(String methodName) {
    if (!ensureJittedAndPolymorphicInline566(methodName)) {
      throw new Error("Didn't find an inlined method in " + methodName);
    }
  }

  public static native boolean ensureJittedAndPolymorphicInline566(String methodName);
  public static native void ensureJitBaselineCompiled(Class<?> cls, String methodName);

  public void increment() {
    field.getClass(); // null check to ensure we get an inlined frame in the CodeInfo
    counter++;
  }
  public static int counter = 0;
}

class Subclass extends Main {
}

class OtherSubclass extends Main {
  public Class<?> sameInvokeVirtual() {
    return OtherSubclass.class;
  }

  public Class<?> sameInvokeInterface() {
    return OtherSubclass.class;
  }

  public Class<?> sameInvokeInterface2() {
    return null;
  }
  public Class<?> sameInvokeInterface3() {
    return null;
  }
}
