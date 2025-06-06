/*
 * Copyright (C) 2015 The Android Open Source Project
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

import java.lang.ref.PhantomReference;
import java.lang.ref.ReferenceQueue;
import java.lang.ref.SoftReference;
import java.lang.ref.WeakReference;
import libcore.util.NativeAllocationRegistry;
import android.graphics.Bitmap;

// We take a heap dump that includes a single instance of this
// DumpedStuff class. Objects stored as fields in this class can be easily
// found in the hprof dump by searching for the instance of the DumpedStuff
// class and reading the desired field.
public class DumpedStuff extends SuperDumpedStuff {
  private void allocateObjectAtKnownSite() {
    objectAllocatedAtKnownSite = new Object();
    allocateObjectAtKnownSubSite();
    allocateObjectAtObfSuperSite();
    allocateObjectAtUnObfSuperSite();
    allocateObjectAtOverriddenSite();
  }

  private void allocateObjectAtKnownSubSite() {
    objectAllocatedAtKnownSubSite = new Object();
  }

  public void allocateObjectAtOverriddenSite() {
    objectAllocatedAtOverriddenSite = new Object();
  }

  DumpedStuff(boolean baseline) {
    allocateObjectAtKnownSite();

    int n = baseline ? 400000 : 1000000;
    bigArray = new byte[n];
    for (int i = 0; i < n; i++) {
      bigArray[i] = (byte)((i * i) & 0xFF);
    }

    // 50000, 0xABCDABCD, and 0xBCDABCDA are arbitrary values.
    NativeAllocationRegistry registry =
        new NativeAllocationRegistry(Main.class.getClassLoader(), getNoopFreeFunction(), 50000);
    registry.registerNativeAllocation(anObject, 0xABCDABCD);

    {
      aCleanerThunk = registry.registerNativeAllocation(aCleanedObject, 0xBCDABCDA);
      aCleanerThunk.run();
    }

    {
      Object object = new Object();
      aLongStrongPathToSamplePathObject = new Reference(new Reference(new Reference(object)));
      aShortWeakPathToSamplePathObject = new WeakReference(new Reference(object));
    }

    addedObject = baseline ? null : new AddedObject();
    removedObject = baseline ? new RemovedObject() : null;
    modifiedObject = new ModifiedObject();
    modifiedObject.value = baseline ? 5 : 8;
    modifiedObject.modifiedRefField = baseline ? "A1" : "A2";
    modifiedObject.unmodifiedRefField = "B";
    modifiedStaticField = baseline ? "C1" : "C2";
    modifiedArray = baseline ? new int[]{0, 1, 2, 3} : new int[]{3, 1, 2, 0};

    // Deep matching dominator trees shouldn't smash the stack when we try
    // to diff them. Make some deep dominator trees to help test it.
    for (int i = 0; i < 10000; i++) {
      StackSmasher smasher = new StackSmasher();
      smasher.child = stackSmasher;
      stackSmasher = smasher;

      if (!baseline) {
        smasher = new StackSmasher();
        smasher.child = stackSmasherAdded;
        stackSmasherAdded = smasher;
      }
    }

    gcPathArray[2].right.left = gcPathArray[2].left.right;

    bitmapOne = new Bitmap(100, 200, 0xDEADBEEF, bigArray);
    bitmapTwo = new Bitmap(100, 200, 0xBEEFDEAD, bigArray);
  }

  public static class ObjectTree {
    public ObjectTree left;
    public ObjectTree right;

    public ObjectTree(ObjectTree left, ObjectTree right) {
      this.left = left;
      this.right = right;
    }
  }

  public static class AddedObject {
  }

  public static class RemovedObject {
  }

  public static class UnchangedObject {
  }

  public static class ModifiedObject {
    public int value;
    public String modifiedRefField;
    public String unmodifiedRefField;
  }

  public static class StackSmasher {
    public StackSmasher child;
  }

  public static class Reference {
    public Object referent;

    public Reference(Object referent) {
      this.referent = referent;
    }
  }

  public interface IDumpedManager {
    public static class Stub extends android.os.Binder implements IDumpedManager {
      private static final java.lang.String DESCRIPTOR = "DumpedStuff$IDumpedManager";
      public Stub() {
        super(DESCRIPTOR);
      }
      public static class Proxy implements IDumpedManager {
        android.os.IBinder mRemote;
        Proxy(android.os.IBinder binderProxy) {
          mRemote = binderProxy;
        }
      }
    }
  }

  public interface IBinderInterfaceImpostor {
    public static class Stub {
      public static class Proxy implements IBinderInterfaceImpostor {
        android.os.IBinder mFakeRemote = new android.os.BinderProxy();
        Proxy(android.os.IBinder binderProxy) {
          mFakeRemote = binderProxy;
        }
      }
    }
  }

  private static class BinderProxyCarrier {
    android.os.IBinder mRemote;
    BinderProxyCarrier(android.os.IBinder binderProxy) {
      mRemote = binderProxy;
    }
  }

  private static class BinderService extends IDumpedManager.Stub {
    // Intentionally empty
  };

  private static class FakeBinderService extends IBinderInterfaceImpostor.Stub {
    // Intentionally empty
  };

  private static class Unreachable {
    public Object anchor;
    public Object self;

    public Unreachable(Object anchor) {
      this.self = this;
      this.anchor = anchor;
    }
  }

  public String basicString = "hello, world";
  public String nonAscii = "Sigma (Ʃ) is not ASCII";
  public String embeddedZero = "embedded\0...";  // Non-ASCII for string compression purposes.
  public char[] charArray = "char thing".toCharArray();
  public byte[] byteString = new byte[] {'h', 'i', '.', '\n', '\0', '\0', '\0'};
  public byte[] byteNotString = new byte[] {0, 1, 2, 3, 4, 5};
  public byte[] byteEmpty = new byte[] {};
  public String nullString = null;
  public Object anObject = new Object();
  public Object aCleanedObject = new Object();
  public Runnable aCleanerThunk;
  public Reference aReference = new Reference(anObject);
  public ReferenceQueue<Object> referenceQueue = new ReferenceQueue<Object>();
  public PhantomReference aPhantomReference = new PhantomReference(anObject, referenceQueue);
  public WeakReference aWeakReference = new WeakReference(anObject, referenceQueue);
  public WeakReference aNullReferentReference = new WeakReference(null, referenceQueue);
  public SoftReference aSoftReference = new SoftReference(new Object());
  public Reference reachabilityReferenceChain;
  public byte[] bigArray;
  public Bitmap bitmapOne = null;
  public Bitmap bitmapTwo = null;
  public ObjectTree[] gcPathArray = new ObjectTree[]{null, null,
    new ObjectTree(
        new ObjectTree(null, new ObjectTree(null, null)),
        new ObjectTree(null, null)),
    null};
  public Reference aLongStrongPathToSamplePathObject;
  public WeakReference aShortWeakPathToSamplePathObject;
  public WeakReference aWeakRefToGcRoot = new WeakReference(Main.class);
  public SoftReference aSoftChain = new SoftReference(new Reference(new Reference(new Object())));
  public Object[] basicStringRef;
  public AddedObject addedObject;
  public UnchangedObject unchangedObject = new UnchangedObject();
  public RemovedObject removedObject;
  public ModifiedObject modifiedObject;
  public StackSmasher stackSmasher;
  public StackSmasher stackSmasherAdded;
  public static String modifiedStaticField;
  public int[] modifiedArray;
  public Object objectAllocatedAtKnownSite;
  public Object objectAllocatedAtKnownSubSite;
  public android.os.IBinder correctBinderProxy = new android.os.BinderProxy();
  public android.os.IBinder imposedBinderProxy = new android.os.BinderProxy();
  public android.os.IBinder carriedBinderProxy = new android.os.BinderProxy();
  Object correctBinderProxyObject = new IDumpedManager.Stub.Proxy(correctBinderProxy);
  Object impostorBinderProxyObject = new IBinderInterfaceImpostor.Stub.Proxy(imposedBinderProxy);
  Object carrierBinderProxyObject = new BinderProxyCarrier(carriedBinderProxy);

  Object binderService = new BinderService();
  Object fakeBinderService = new FakeBinderService();
  Object binderToken = new android.os.Binder();
  Object namedBinderToken = new android.os.Binder("awesomeToken");

  Object unreachableAnchor = new Object();

  // Allocate those objects that we need to not be GC'd before taking the heap
  // dump.
  public void shouldNotGc() {
    reachabilityReferenceChain = new Reference(
        new SoftReference(
        new Reference(
        new WeakReference(
        new SoftReference(
        new PhantomReference(new Object(), referenceQueue))))));

    new Unreachable(unreachableAnchor);
  }

  static {
    System.loadLibrary("ahat-test-jni");
  }

  private static native long getNoopFreeFunction();
}
