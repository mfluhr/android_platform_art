/*
 * Copyright (C) 2017 The Android Open Source Project
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

package art;

import java.lang.reflect.Field;
import org.apache.harmony.dalvik.ddmc.*;
import dalvik.system.VMDebug;

import java.lang.reflect.Method;
import java.util.Arrays;
import java.util.function.*;
import java.util.zip.Adler32;
import java.nio.*;

public class Test1940 {
  public static final int DDMS_TYPE_INDEX = 0;
  public static final int DDMS_LEN_INDEX = 4;
  public static final int DDMS_HEADER_LENGTH = 8;
  public static final int MY_DDMS_TYPE = 0xDEADBEEF;
  public static final int MY_DDMS_RESPONSE_TYPE = 0xFADE7357;
  public static final int MY_EMPTY_DDMS_TYPE = 0xABCDEF01;
  public static final int MY_INVALID_DDMS_TYPE = 0x12345678;

  public static final boolean PRINT_ALL_CHUNKS = false;

  public static interface DdmHandler {
    public void HandleChunk(int type, byte[] data) throws Exception;
  }

  public static final class TestError extends Error {
    public TestError(String s) { super(s); }
  }

  private static void checkEq(Object a, Object b) {
    if (!a.equals(b)) {
      throw new TestError("Failure: " + a + " != " + b);
    }
  }

  private static boolean chunkEq(Chunk c1, Chunk c2) {
    ChunkWrapper a = new ChunkWrapper(c1);
    ChunkWrapper b = new ChunkWrapper(c2);
    return a.type() == b.type() &&
           a.offset() == b.offset() &&
           a.length() == b.length() &&
           Arrays.equals(a.data(), b.data());
  }

  private static String printChunk(Chunk k) {
    ChunkWrapper c = new ChunkWrapper(k);
    byte[] out = new byte[c.length()];
    System.arraycopy(c.data(), c.offset(), out, 0, c.length());
    return String.format("Chunk(Type: 0x%X, Len: %d, data: %s)",
        c.type(), c.length(), Arrays.toString(out));
  }

  private static final class MyDdmHandler extends ChunkHandler {
    public void onConnected() {}
    public void onDisconnected() {}
    public Chunk handleChunk(Chunk req) {
      System.out.println("MyDdmHandler: Chunk received: " + printChunk(req));
      if (req.type == MY_DDMS_TYPE) {
        // For this test we will simply calculate the checksum
        ByteBuffer b = ByteBuffer.wrap(new byte[8]);
        Adler32 a = new Adler32();
        ChunkWrapper reqWrapper = new ChunkWrapper(req);
        a.update(reqWrapper.data(), reqWrapper.offset(), reqWrapper.length());
        b.order(ByteOrder.BIG_ENDIAN);
        long val = a.getValue();
        b.putLong(val);
        System.out.printf("MyDdmHandler: Putting value 0x%X\n", val);
        Chunk ret = new Chunk(MY_DDMS_RESPONSE_TYPE, b.array(), 0, 8);
        System.out.println("MyDdmHandler: Chunk returned: " + printChunk(ret));
        return ret;
      } else if (req.type == MY_EMPTY_DDMS_TYPE) {
        return new Chunk(MY_DDMS_RESPONSE_TYPE, new byte[0], 0, 0);
      } else if (req.type == MY_INVALID_DDMS_TYPE) {
        // This is a very invalid chunk.
        return new Chunk(MY_DDMS_RESPONSE_TYPE, new byte[] { 0 }, /*offset*/ 12, /*length*/ 55);
      } else {
        throw new TestError("Unknown ddm request type: " + req.type);
      }
    }
  }


  /**
   * Wrapper for accessing the hidden fields in {@link Chunk} in CTS.
   */
  private static class ChunkWrapper {
    private Chunk c;

    ChunkWrapper(Chunk c) {
      this.c = c;
    }

    int type() {
      return c.type;
    }

    int length() {
      try {
        Field f = Chunk.class.getField("length");
        return (int) f.get(c);
      } catch (NoSuchFieldException | IllegalAccessException e) {
        throw new RuntimeException(e);
      }
    }

    byte[] data() {
      try {
        Field f = Chunk.class.getField("data");
        return (byte[]) f.get(c);
      } catch (NoSuchFieldException | IllegalAccessException e) {
        throw new RuntimeException(e);
      }
    }

    int offset() {
      try {
        Field f = Chunk.class.getField("offset");
        return (int) f.get(c);
      } catch (NoSuchFieldException | IllegalAccessException e) {
        throw new RuntimeException(e);
      }
    }
  }

  public static final ChunkHandler SINGLE_HANDLER = new MyDdmHandler();

  public static DdmHandler CURRENT_HANDLER;

  public static void HandlePublish(int type, byte[] data) throws Exception {
    if (PRINT_ALL_CHUNKS) {
      System.out.println(
          "Unknown Chunk published: " + printChunk(new Chunk(type, data, 0, data.length)));
    }
    CURRENT_HANDLER.HandleChunk(type, data);
  }

  // TYPE Thread Create
  public static final int TYPE_THCR = 0x54484352;
  // Type Thread name
  public static final int TYPE_THNM = 0x54484E4D;
  // Type Thread death.
  public static final int TYPE_THDE = 0x54484445;
  // Type Heap info
  public static final int TYPE_HPIF = 0x48504946;
  // Type Trace Results
  public static final int TYPE_MPSE = 0x4D505345;

  public static boolean IsFromThread(Thread t, byte[] data) {
    // DDMS always puts the thread-id as the first 4 bytes.
    ByteBuffer b = ByteBuffer.wrap(data);
    b.order(ByteOrder.BIG_ENDIAN);
    return b.getInt() == (int) t.getId();
  }

  public static final class AwaitChunkHandler implements DdmHandler {
    public final Predicate<Chunk> needle;
    public final DdmHandler chain;
    private boolean found = false;
    public AwaitChunkHandler(Predicate<Chunk> needle, DdmHandler chain) {
      this.needle = needle;
      this.chain = chain;
    }
    public void HandleChunk(int type, byte[] data) throws Exception {
      chain.HandleChunk(type, data);
      Chunk c = new Chunk(type, data, 0, data.length);
      if (needle.test(c)) {
        synchronized (this) {
          found = true;
          notifyAll();
        }
      }
    }
    public synchronized void Await() throws Exception {
      while (!found) {
        wait();
      }
    }
  }

  public static void run() throws Exception {
    DdmHandler BaseHandler = (type, data) -> {
      System.out.println("Chunk published: " + printChunk(new Chunk(type, data, 0, data.length)));
    };
    CURRENT_HANDLER = BaseHandler;
    initializeTest();
    Method publish = Test1940.class.getDeclaredMethod("HandlePublish",
                                                      Integer.TYPE,
                                                      new byte[0].getClass());
    Thread listener = new Thread(() -> { Test1940.publishListen(publish); });
    listener.setDaemon(true);
    listener.start();
    // Test sending chunk directly.
    DdmServer.registerHandler(MY_DDMS_TYPE, SINGLE_HANDLER);
    DdmServer.registerHandler(MY_EMPTY_DDMS_TYPE, SINGLE_HANDLER);
    DdmServer.registerHandler(MY_INVALID_DDMS_TYPE, SINGLE_HANDLER);
    DdmServer.registrationComplete();
    byte[] data = new byte[] { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    System.out.println("Sending data " + Arrays.toString(data));
    Chunk res = processChunk(data);
    System.out.println("JVMTI returned chunk: " + printChunk(res));

    // Test sending an empty chunk.
    System.out.println("Sending empty data array");
    res = processChunk(new byte[0]);
    System.out.println("JVMTI returned chunk: " + printChunk(res));

    // Test sending chunk through DdmServer#sendChunk
    Chunk c = new Chunk(
        MY_DDMS_TYPE, new byte[] { 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10 }, 0, 8);
    AwaitChunkHandler h = new AwaitChunkHandler((x) -> chunkEq(c, x), CURRENT_HANDLER);
    CURRENT_HANDLER = h;
    System.out.println("Sending chunk: " + printChunk(c));
    DdmServer.sendChunk(c);
    h.Await();
    CURRENT_HANDLER = BaseHandler;

    // Test getting back an empty chunk.
    data = new byte[] { 0x1 };
    System.out.println(
        "Sending data " + Arrays.toString(data) + " to chunk handler " + MY_EMPTY_DDMS_TYPE);
    res = processChunk(new Chunk(MY_EMPTY_DDMS_TYPE, data, 0, 1));
    System.out.println("JVMTI returned chunk: " + printChunk(res));

    // Test getting back an invalid chunk.
    System.out.println(
        "Sending data " + Arrays.toString(data) + " to chunk handler " + MY_INVALID_DDMS_TYPE);
    try {
      res = processChunk(new Chunk(MY_INVALID_DDMS_TYPE, data, 0, 1));
      System.out.println("JVMTI returned chunk: " + printChunk(res));
    } catch (RuntimeException e) {
      System.out.println("Got error: " + e.getMessage());
    }

    // Test thread chunks are sent.
    final boolean[] types_seen = new boolean[] { false, false, false };
    AwaitChunkHandler wait_thd= new AwaitChunkHandler(
      (x) -> types_seen[0] && types_seen[1] && types_seen[2],
      (type, cdata) -> {
        switch (type) {
          case TYPE_THCR:
            types_seen[0] = true;
            break;
          case TYPE_THNM:
            types_seen[1] = true;
            break;
          case TYPE_THDE:
            types_seen[2] = true;
            break;
          default:
            // We don't want to print other types.
            break;
        }
      });
    CURRENT_HANDLER = wait_thd;
    DdmVmInternal.setThreadNotifyEnabled(true);
    System.out.println("threadNotify started!");
    final Thread thr = new Thread(() -> { return; }, "THREAD");
    thr.start();
    System.out.println("Target thread started!");
    thr.join();
    System.out.println("Target thread finished!");
    DdmVmInternal.setThreadNotifyEnabled(false);
    System.out.println("threadNotify Disabled!");
    wait_thd.Await();
    // Make sure we saw at least one of Thread-create, Thread name, & thread death.
    if (!types_seen[0] || !types_seen[1] || !types_seen[2]) {
      System.out.println("Didn't see expected chunks for thread creation! got: " +
          Arrays.toString(types_seen));
    } else {
      System.out.println("Saw expected thread events.");
    }

    // method Tracing
    AwaitChunkHandler mpse = new AwaitChunkHandler((x) -> x.type == TYPE_MPSE, (type, cdata) -> {
      // This chunk includes timing and thread information so we just check the type.
      if (type == TYPE_MPSE) {
        System.out.println("Expected chunk type published: " + type);
      }
    });
    CURRENT_HANDLER = mpse;
    VMDebug.startMethodTracingDdms(/*size: default*/0,
                                   /*flags: none*/ 0,
                                   /*sampling*/ false,
                                   /*interval*/ 0);
    doNothing();
    doNothing();
    doNothing();
    doNothing();
    VMDebug.stopMethodTracing();
    mpse.Await();
  }

  private static void doNothing() {}
  private static Chunk processChunk(byte[] val) {
    return processChunk(new Chunk(MY_DDMS_TYPE, val, 0, val.length));
  }

  private static native void initializeTest();
  private static native Chunk processChunk(Chunk val);
  private static native void publishListen(Method publish);
}
