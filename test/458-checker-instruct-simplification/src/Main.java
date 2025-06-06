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

import java.lang.reflect.Method;

public class Main {

  public static void assertBooleanEquals(boolean expected, boolean result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertIntEquals(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertLongEquals(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertFloatEquals(float expected, float result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertDoubleEquals(double expected, double result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertStringEquals(String expected, String result) {
    if (expected == null ? result != null : !expected.equals(result)) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  /**
   * Tiny programs exercising optimizations of arithmetic identities.
   */

  /// CHECK-START: long Main.$noinline$Add0(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Const0:j\d+>>  LongConstant 0
  /// CHECK-DAG:     <<Add:j\d+>>     Add [<<Const0>>,<<Arg>>]
  /// CHECK-DAG:                      Return [<<Add>>]

  /// CHECK-START: long Main.$noinline$Add0(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>     ParameterValue
  /// CHECK-DAG:                      Return [<<Arg>>]

  /// CHECK-START: long Main.$noinline$Add0(long) instruction_simplifier (after)
  /// CHECK-NOT:                        Add

  public static long $noinline$Add0(long arg) {
    return 0 + arg;
  }

  /// CHECK-START: int Main.$noinline$AddAddSubAddConst(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<ArgValue:i\d+>>  ParameterValue
  /// CHECK-DAG:     <<Const1:i\d+>>    IntConstant 1
  /// CHECK-DAG:     <<Const2:i\d+>>    IntConstant 2
  /// CHECK-DAG:     <<ConstM3:i\d+>>   IntConstant -3
  /// CHECK-DAG:     <<Const4:i\d+>>    IntConstant 4
  /// CHECK-DAG:     <<Add1:i\d+>>      Add [<<ArgValue>>,<<Const1>>]
  /// CHECK-DAG:     <<Add2:i\d+>>      Add [<<Add1>>,<<Const2>>]
  /// CHECK-DAG:     <<Add3:i\d+>>      Add [<<Add2>>,<<ConstM3>>]
  /// CHECK-DAG:     <<Add4:i\d+>>      Add [<<Add3>>,<<Const4>>]
  /// CHECK-DAG:                        Return [<<Add4>>]

  /// CHECK-START: int Main.$noinline$AddAddSubAddConst(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<ArgValue:i\d+>>  ParameterValue
  /// CHECK-DAG:     <<Const4:i\d+>>    IntConstant 4
  /// CHECK-DAG:     <<Add:i\d+>>       Add [<<ArgValue>>,<<Const4>>]
  /// CHECK-DAG:                        Return [<<Add>>]

  public static int $noinline$AddAddSubAddConst(int arg) {
    return arg + 1 + 2 - 3 + 4;
  }

  /// CHECK-START: int Main.$noinline$AndAllOnes(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<ConstF:i\d+>>  IntConstant -1
  /// CHECK-DAG:     <<And:i\d+>>     And [<<Arg>>,<<ConstF>>]
  /// CHECK-DAG:                      Return [<<And>>]

  /// CHECK-START: int Main.$noinline$AndAllOnes(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>     ParameterValue
  /// CHECK-DAG:                      Return [<<Arg>>]

  /// CHECK-START: int Main.$noinline$AndAllOnes(int) instruction_simplifier (after)
  /// CHECK-NOT:                      And

  public static int $noinline$AndAllOnes(int arg) {
    return arg & -1;
  }

  /// CHECK-START: int Main.$noinline$UShr28And15(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const28:i\d+>>  IntConstant 28
  /// CHECK-DAG:     <<Const15:i\d+>>  IntConstant 15
  /// CHECK-DAG:     <<UShr:i\d+>>     UShr [<<Arg>>,<<Const28>>]
  /// CHECK-DAG:     <<And:i\d+>>      And [<<UShr>>,<<Const15>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: int Main.$noinline$UShr28And15(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const28:i\d+>>  IntConstant 28
  /// CHECK-DAG:     <<UShr:i\d+>>     UShr [<<Arg>>,<<Const28>>]
  /// CHECK-DAG:                       Return [<<UShr>>]

  /// CHECK-START: int Main.$noinline$UShr28And15(int) instruction_simplifier (after)
  /// CHECK-NOT:                       And

  public static int $noinline$UShr28And15(int arg) {
    return (arg >>> 28) & 15;
  }

  /// CHECK-START: long Main.$noinline$UShr60And15(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const60:i\d+>>  IntConstant 60
  /// CHECK-DAG:     <<Const15:j\d+>>  LongConstant 15
  /// CHECK-DAG:     <<UShr:j\d+>>     UShr [<<Arg>>,<<Const60>>]
  /// CHECK-DAG:     <<And:j\d+>>      And [<<UShr>>,<<Const15>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: long Main.$noinline$UShr60And15(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const60:i\d+>>  IntConstant 60
  /// CHECK-DAG:     <<UShr:j\d+>>     UShr [<<Arg>>,<<Const60>>]
  /// CHECK-DAG:                       Return [<<UShr>>]

  /// CHECK-START: long Main.$noinline$UShr60And15(long) instruction_simplifier (after)
  /// CHECK-NOT:                       And

  public static long $noinline$UShr60And15(long arg) {
    return (arg >>> 60) & 15;
  }

  /// CHECK-START: int Main.$noinline$UShr28And7(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const28:i\d+>>  IntConstant 28
  /// CHECK-DAG:     <<Const7:i\d+>>   IntConstant 7
  /// CHECK-DAG:     <<UShr:i\d+>>     UShr [<<Arg>>,<<Const28>>]
  /// CHECK-DAG:     <<And:i\d+>>      And [<<UShr>>,<<Const7>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: int Main.$noinline$UShr28And7(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const28:i\d+>>  IntConstant 28
  /// CHECK-DAG:     <<Const7:i\d+>>   IntConstant 7
  /// CHECK-DAG:     <<UShr:i\d+>>     UShr [<<Arg>>,<<Const28>>]
  /// CHECK-DAG:     <<And:i\d+>>      And [<<UShr>>,<<Const7>>]
  /// CHECK-DAG:                       Return [<<And>>]

  public static int $noinline$UShr28And7(int arg) {
    return (arg >>> 28) & 7;
  }

  /// CHECK-START: long Main.$noinline$UShr60And7(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const60:i\d+>>  IntConstant 60
  /// CHECK-DAG:     <<Const7:j\d+>>   LongConstant 7
  /// CHECK-DAG:     <<UShr:j\d+>>     UShr [<<Arg>>,<<Const60>>]
  /// CHECK-DAG:     <<And:j\d+>>      And [<<UShr>>,<<Const7>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: long Main.$noinline$UShr60And7(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const60:i\d+>>  IntConstant 60
  /// CHECK-DAG:     <<Const7:j\d+>>   LongConstant 7
  /// CHECK-DAG:     <<UShr:j\d+>>     UShr [<<Arg>>,<<Const60>>]
  /// CHECK-DAG:     <<And:j\d+>>      And [<<UShr>>,<<Const7>>]
  /// CHECK-DAG:                       Return [<<And>>]

  public static long $noinline$UShr60And7(long arg) {
    return (arg >>> 60) & 7;
  }

  /// CHECK-START: int Main.$noinline$Shr24And255(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const24:i\d+>>  IntConstant 24
  /// CHECK-DAG:     <<Const255:i\d+>> IntConstant 255
  /// CHECK-DAG:     <<Shr:i\d+>>      Shr [<<Arg>>,<<Const24>>]
  /// CHECK-DAG:     <<And:i\d+>>      And [<<Shr>>,<<Const255>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: int Main.$noinline$Shr24And255(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const24:i\d+>>  IntConstant 24
  /// CHECK-DAG:     <<UShr:i\d+>>     UShr [<<Arg>>,<<Const24>>]
  /// CHECK-DAG:                       Return [<<UShr>>]

  /// CHECK-START: int Main.$noinline$Shr24And255(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Shr
  /// CHECK-NOT:                       And

  public static int $noinline$Shr24And255(int arg) {
    return (arg >> 24) & 255;
  }

  /// CHECK-START: int Main.$noinline$Shr25And127(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const25:i\d+>>  IntConstant 25
  /// CHECK-DAG:     <<Const127:i\d+>> IntConstant 127
  /// CHECK-DAG:     <<Shr:i\d+>>      Shr [<<Arg>>,<<Const25>>]
  /// CHECK-DAG:     <<And:i\d+>>      And [<<Shr>>,<<Const127>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: int Main.$noinline$Shr25And127(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const25:i\d+>>  IntConstant 25
  /// CHECK-DAG:     <<UShr:i\d+>>     UShr [<<Arg>>,<<Const25>>]
  /// CHECK-DAG:                       Return [<<UShr>>]

  /// CHECK-START: int Main.$noinline$Shr25And127(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Shr
  /// CHECK-NOT:                       And

  public static int $noinline$Shr25And127(int arg) {
    return (arg >> 25) & 127;
  }

  /// CHECK-START: long Main.$noinline$Shr56And255(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const56:i\d+>>  IntConstant 56
  /// CHECK-DAG:     <<Const255:j\d+>> LongConstant 255
  /// CHECK-DAG:     <<Shr:j\d+>>      Shr [<<Arg>>,<<Const56>>]
  /// CHECK-DAG:     <<And:j\d+>>      And [<<Shr>>,<<Const255>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: long Main.$noinline$Shr56And255(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const56:i\d+>>  IntConstant 56
  /// CHECK-DAG:     <<UShr:j\d+>>     UShr [<<Arg>>,<<Const56>>]
  /// CHECK-DAG:                       Return [<<UShr>>]

  /// CHECK-START: long Main.$noinline$Shr56And255(long) instruction_simplifier (after)
  /// CHECK-NOT:                       Shr
  /// CHECK-NOT:                       And

  public static long $noinline$Shr56And255(long arg) {
    return (arg >> 56) & 255;
  }

  /// CHECK-START: long Main.$noinline$Shr57And127(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const57:i\d+>>  IntConstant 57
  /// CHECK-DAG:     <<Const127:j\d+>> LongConstant 127
  /// CHECK-DAG:     <<Shr:j\d+>>      Shr [<<Arg>>,<<Const57>>]
  /// CHECK-DAG:     <<And:j\d+>>      And [<<Shr>>,<<Const127>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: long Main.$noinline$Shr57And127(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const57:i\d+>>  IntConstant 57
  /// CHECK-DAG:     <<UShr:j\d+>>     UShr [<<Arg>>,<<Const57>>]
  /// CHECK-DAG:                       Return [<<UShr>>]

  /// CHECK-START: long Main.$noinline$Shr57And127(long) instruction_simplifier (after)
  /// CHECK-NOT:                       Shr
  /// CHECK-NOT:                       And

  public static long $noinline$Shr57And127(long arg) {
    return (arg >> 57) & 127;
  }

  /// CHECK-START: int Main.$noinline$Shr24And127(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const24:i\d+>>  IntConstant 24
  /// CHECK-DAG:     <<Const127:i\d+>> IntConstant 127
  /// CHECK-DAG:     <<Shr:i\d+>>      Shr [<<Arg>>,<<Const24>>]
  /// CHECK-DAG:     <<And:i\d+>>      And [<<Shr>>,<<Const127>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: int Main.$noinline$Shr24And127(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const24:i\d+>>  IntConstant 24
  /// CHECK-DAG:     <<Const127:i\d+>> IntConstant 127
  /// CHECK-DAG:     <<Shr:i\d+>>      Shr [<<Arg>>,<<Const24>>]
  /// CHECK-DAG:     <<And:i\d+>>      And [<<Shr>>,<<Const127>>]
  /// CHECK-DAG:                       Return [<<And>>]

  public static int $noinline$Shr24And127(int arg) {
    return (arg >> 24) & 127;
  }

  /// CHECK-START: long Main.$noinline$Shr56And127(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const56:i\d+>>  IntConstant 56
  /// CHECK-DAG:     <<Const127:j\d+>> LongConstant 127
  /// CHECK-DAG:     <<Shr:j\d+>>      Shr [<<Arg>>,<<Const56>>]
  /// CHECK-DAG:     <<And:j\d+>>      And [<<Shr>>,<<Const127>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: long Main.$noinline$Shr56And127(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const56:i\d+>>  IntConstant 56
  /// CHECK-DAG:     <<Const127:j\d+>> LongConstant 127
  /// CHECK-DAG:     <<Shr:j\d+>>      Shr [<<Arg>>,<<Const56>>]
  /// CHECK-DAG:     <<And:j\d+>>      And [<<Shr>>,<<Const127>>]
  /// CHECK-DAG:                       Return [<<And>>]

  public static long $noinline$Shr56And127(long arg) {
    return (arg >> 56) & 127;
  }

  /// CHECK-START: long Main.$noinline$Div1(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Const1:j\d+>>  LongConstant 1
  /// CHECK-DAG:     <<Div:j\d+>>     Div [<<Arg>>,<<Const1>>]
  /// CHECK-DAG:                      Return [<<Div>>]

  /// CHECK-START: long Main.$noinline$Div1(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>     ParameterValue
  /// CHECK-DAG:                      Return [<<Arg>>]

  /// CHECK-START: long Main.$noinline$Div1(long) instruction_simplifier (after)
  /// CHECK-NOT:                      Div

  public static long $noinline$Div1(long arg) {
    return arg / 1;
  }

  /// CHECK-START: int Main.$noinline$DivN1(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<ConstN1:i\d+>>  IntConstant -1
  /// CHECK-DAG:     <<Div:i\d+>>      Div [<<Arg>>,<<ConstN1>>]
  /// CHECK-DAG:                       Return [<<Div>>]

  /// CHECK-START: int Main.$noinline$DivN1(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Arg>>]
  /// CHECK-DAG:                       Return [<<Neg>>]

  /// CHECK-START: int Main.$noinline$DivN1(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Div

  public static int $noinline$DivN1(int arg) {
    return arg / -1;
  }

  /// CHECK-START: long Main.$noinline$Mul1(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Const1:j\d+>>  LongConstant 1
  /// CHECK-DAG:     <<Mul:j\d+>>     Mul [<<Const1>>,<<Arg>>]
  /// CHECK-DAG:                      Return [<<Mul>>]

  /// CHECK-START: long Main.$noinline$Mul1(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>     ParameterValue
  /// CHECK-DAG:                      Return [<<Arg>>]

  /// CHECK-START: long Main.$noinline$Mul1(long) instruction_simplifier (after)
  /// CHECK-NOT:                       Mul

  public static long $noinline$Mul1(long arg) {
    return arg * 1;
  }

  /// CHECK-START: int Main.$noinline$MulN1(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<ConstN1:i\d+>>  IntConstant -1
  /// CHECK-DAG:     <<Mul:i\d+>>      Mul [<<Arg>>,<<ConstN1>>]
  /// CHECK-DAG:                       Return [<<Mul>>]

  /// CHECK-START: int Main.$noinline$MulN1(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Arg>>]
  /// CHECK-DAG:                       Return [<<Neg>>]

  /// CHECK-START: int Main.$noinline$MulN1(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Mul

  public static int $noinline$MulN1(int arg) {
    return arg * -1;
  }

  /// CHECK-START: long Main.$noinline$MulPowerOfTwo128(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>       ParameterValue
  /// CHECK-DAG:     <<Const128:j\d+>>  LongConstant 128
  /// CHECK-DAG:     <<Mul:j\d+>>       Mul [<<Const128>>,<<Arg>>]
  /// CHECK-DAG:                        Return [<<Mul>>]

  /// CHECK-START: long Main.$noinline$MulPowerOfTwo128(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>       ParameterValue
  /// CHECK-DAG:     <<Const7:i\d+>>    IntConstant 7
  /// CHECK-DAG:     <<Shl:j\d+>>       Shl [<<Arg>>,<<Const7>>]
  /// CHECK-DAG:                        Return [<<Shl>>]

  /// CHECK-START: long Main.$noinline$MulPowerOfTwo128(long) instruction_simplifier (after)
  /// CHECK-NOT:                        Mul

  public static long $noinline$MulPowerOfTwo128(long arg) {
    return arg * 128;
  }

  /// CHECK-START: long Main.$noinline$MulMulMulConst(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<ArgValue:j\d+>>  ParameterValue
  /// CHECK-DAG:     <<Const10:j\d+>>   LongConstant 10
  /// CHECK-DAG:     <<Const11:j\d+>>   LongConstant 11
  /// CHECK-DAG:     <<Const12:j\d+>>   LongConstant 12
  /// CHECK-DAG:     <<Mul1:j\d+>>      Mul [<<Const10>>,<<ArgValue>>]
  /// CHECK-DAG:     <<Mul2:j\d+>>      Mul [<<Mul1>>,<<Const11>>]
  /// CHECK-DAG:     <<Mul3:j\d+>>      Mul [<<Mul2>>,<<Const12>>]
  /// CHECK-DAG:                        Return [<<Mul3>>]

  /// CHECK-START: long Main.$noinline$MulMulMulConst(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<ArgValue:j\d+>>   ParameterValue
  /// CHECK-DAG:     <<Const1320:j\d+>>  LongConstant 1320
  /// CHECK-DAG:     <<Mul:j\d+>>        Mul [<<ArgValue>>,<<Const1320>>]
  /// CHECK-DAG:                         Return [<<Mul>>]

  public static long $noinline$MulMulMulConst(long arg) {
    return 10 * arg * 11 * 12;
  }

  /// CHECK-START: int Main.$noinline$Or0(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:     <<Or:i\d+>>       Or [<<Arg>>,<<Const0>>]
  /// CHECK-DAG:                       Return [<<Or>>]

  /// CHECK-START: int Main.$noinline$Or0(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: int Main.$noinline$Or0(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Or

  public static int $noinline$Or0(int arg) {
    return arg | 0;
  }

  /// CHECK-START: long Main.$noinline$OrSame(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>       ParameterValue
  /// CHECK-DAG:     <<Or:j\d+>>        Or [<<Arg>>,<<Arg>>]
  /// CHECK-DAG:                        Return [<<Or>>]

  /// CHECK-START: long Main.$noinline$OrSame(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>       ParameterValue
  /// CHECK-DAG:                        Return [<<Arg>>]

  /// CHECK-START: long Main.$noinline$OrSame(long) instruction_simplifier (after)
  /// CHECK-NOT:                        Or

  public static long $noinline$OrSame(long arg) {
    return arg | arg;
  }

  /// CHECK-START: int Main.$noinline$Shl0(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:     <<Shl:i\d+>>      Shl [<<Arg>>,<<Const0>>]
  /// CHECK-DAG:                       Return [<<Shl>>]

  /// CHECK-START: int Main.$noinline$Shl0(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: int Main.$noinline$Shl0(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Shl

  public static int $noinline$Shl0(int arg) {
    return arg << 0;
  }

  /// CHECK-START: long Main.$noinline$Shr0(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:     <<Shr:j\d+>>      Shr [<<Arg>>,<<Const0>>]
  /// CHECK-DAG:                       Return [<<Shr>>]

  /// CHECK-START: long Main.$noinline$Shr0(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: long Main.$noinline$Shr0(long) instruction_simplifier (after)
  /// CHECK-NOT:                       Shr

  public static long $noinline$Shr0(long arg) {
    return arg >> 0;
  }

  /// CHECK-START: long Main.$noinline$Shr64(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const64:i\d+>>  IntConstant 64
  /// CHECK-DAG:     <<Shr:j\d+>>      Shr [<<Arg>>,<<Const64>>]
  /// CHECK-DAG:                       Return [<<Shr>>]

  /// CHECK-START: long Main.$noinline$Shr64(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: long Main.$noinline$Shr64(long) instruction_simplifier (after)
  /// CHECK-NOT:                       Shr

  public static long $noinline$Shr64(long arg) {
    return arg >> 64;
  }

  /// CHECK-START: long Main.$noinline$Sub0(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:j\d+>>   LongConstant 0
  /// CHECK-DAG:     <<Sub:j\d+>>      Sub [<<Arg>>,<<Const0>>]
  /// CHECK-DAG:                       Return [<<Sub>>]

  /// CHECK-START: long Main.$noinline$Sub0(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: long Main.$noinline$Sub0(long) instruction_simplifier (after)
  /// CHECK-NOT:                       Sub

  public static long $noinline$Sub0(long arg) {
    return arg - 0;
  }

  /// CHECK-START: int Main.$noinline$SubAliasNeg(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:     <<Sub:i\d+>>      Sub [<<Const0>>,<<Arg>>]
  /// CHECK-DAG:                       Return [<<Sub>>]

  /// CHECK-START: int Main.$noinline$SubAliasNeg(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Arg>>]
  /// CHECK-DAG:                       Return [<<Neg>>]

  /// CHECK-START: int Main.$noinline$SubAliasNeg(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Sub

  public static int $noinline$SubAliasNeg(int arg) {
    return 0 - arg;
  }

  /// CHECK-START: int Main.$noinline$SubAddConst1(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<ArgValue:i\d+>>  ParameterValue
  /// CHECK-DAG:     <<Const5:i\d+>>    IntConstant 5
  /// CHECK-DAG:     <<Const6:i\d+>>    IntConstant 6
  /// CHECK-DAG:     <<Sub:i\d+>>       Sub [<<Const5>>,<<ArgValue>>]
  /// CHECK-DAG:     <<Add:i\d+>>       Add [<<Sub>>,<<Const6>>]
  /// CHECK-DAG:                        Return [<<Add>>]

  /// CHECK-START: int Main.$noinline$SubAddConst1(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<ArgValue:i\d+>>  ParameterValue
  /// CHECK-DAG:     <<Const11:i\d+>>   IntConstant 11
  /// CHECK-DAG:     <<Sub:i\d+>>       Sub [<<Const11>>,<<ArgValue>>]
  /// CHECK-DAG:                        Return [<<Sub>>]

  public static int $noinline$SubAddConst1(int arg) {
    return 5 - arg + 6;
  }

  /// CHECK-START: int Main.$noinline$SubAddConst2(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<ArgValue:i\d+>>  ParameterValue
  /// CHECK-DAG:     <<Const14:i\d+>>   IntConstant 14
  /// CHECK-DAG:     <<Const13:i\d+>>   IntConstant 13
  /// CHECK-DAG:     <<Add:i\d+>>       Add [<<ArgValue>>,<<Const13>>]
  /// CHECK-DAG:     <<Sub:i\d+>>       Sub [<<Const14>>,<<Add>>]
  /// CHECK-DAG:                        Return [<<Sub>>]

  /// CHECK-START: int Main.$noinline$SubAddConst2(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<ArgValue:i\d+>>  ParameterValue
  /// CHECK-DAG:     <<Const1:i\d+>>    IntConstant 1
  /// CHECK-DAG:     <<Sub:i\d+>>       Sub [<<Const1>>,<<ArgValue>>]
  /// CHECK-DAG:                        Return [<<Sub>>]

  public static int $noinline$SubAddConst2(int arg) {
    return 14 - (arg + 13);
  }

  /// CHECK-START: long Main.$noinline$SubSubConst(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<ArgValue:j\d+>>  ParameterValue
  /// CHECK-DAG:     <<Const17:j\d+>>   LongConstant 17
  /// CHECK-DAG:     <<Const18:j\d+>>   LongConstant 18
  /// CHECK-DAG:     <<Sub1:j\d+>>      Sub [<<Const18>>,<<ArgValue>>]
  /// CHECK-DAG:     <<Sub2:j\d+>>      Sub [<<Const17>>,<<Sub1>>]
  /// CHECK-DAG:                        Return [<<Sub2>>]

  /// CHECK-START: long Main.$noinline$SubSubConst(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<ArgValue:j\d+>>  ParameterValue
  /// CHECK-DAG:     <<ConstM1:j\d+>>   LongConstant -1
  /// CHECK-DAG:     <<Add:j\d+>>       Add [<<ArgValue>>,<<ConstM1>>]
  /// CHECK-DAG:                        Return [<<Add>>]

  public static long $noinline$SubSubConst(long arg) {
    return 17 - (18 - arg);
  }

  /// CHECK-START: long Main.$noinline$UShr0(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:     <<UShr:j\d+>>     UShr [<<Arg>>,<<Const0>>]
  /// CHECK-DAG:                       Return [<<UShr>>]

  /// CHECK-START: long Main.$noinline$UShr0(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: long Main.$noinline$UShr0(long) instruction_simplifier (after)
  /// CHECK-NOT:                       UShr

  public static long $noinline$UShr0(long arg) {
    return arg >>> 0;
  }

  /// CHECK-START: int Main.$noinline$Xor0(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:     <<Xor:i\d+>>      Xor [<<Arg>>,<<Const0>>]
  /// CHECK-DAG:                       Return [<<Xor>>]

  /// CHECK-START: int Main.$noinline$Xor0(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: int Main.$noinline$Xor0(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Xor

  public static int $noinline$Xor0(int arg) {
    return arg ^ 0;
  }

  /**
   * Test that addition or subtraction operation with both inputs negated are
   * optimized to use a single negation after the operation.
   * The transformation tested is implemented in
   * `InstructionSimplifierVisitor::TryMoveNegOnInputsAfterBinop`.
   */

  /// CHECK-START: int Main.$noinline$AddNegs1(int, int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg1:i\d+>>     Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Neg2:i\d+>>     Neg [<<Arg2>>]
  /// CHECK-DAG:     <<Add:i\d+>>      Add [<<Neg1>>,<<Neg2>>]
  /// CHECK-DAG:                       Return [<<Add>>]

  /// CHECK-START: int Main.$noinline$AddNegs1(int, int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-NOT:                       Neg
  /// CHECK-DAG:     <<Add:i\d+>>      Add [<<Arg1>>,<<Arg2>>]
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Add>>]
  /// CHECK-DAG:                       Return [<<Neg>>]

  public static int $noinline$AddNegs1(int arg1, int arg2) {
    return -arg1 + -arg2;
  }

  /**
   * This is similar to the test-case AddNegs1, but the negations have
   * multiple uses.
   * The transformation tested is implemented in
   * `InstructionSimplifierVisitor::TryMoveNegOnInputsAfterBinop`.
   * The current code won't perform the previous optimization. The
   * transformations do not look at other uses of their inputs. As they don't
   * know what will happen with other uses, they do not take the risk of
   * increasing the register pressure by creating or extending live ranges.
   */

  /// CHECK-START: int Main.$noinline$AddNegs2(int, int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg1:i\d+>>     Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Neg2:i\d+>>     Neg [<<Arg2>>]
  /// CHECK-DAG:     <<Add1:i\d+>>     Add [<<Neg1>>,<<Neg2>>]
  /// CHECK-DAG:     <<Add2:i\d+>>     Add [<<Neg1>>,<<Neg2>>]
  /// CHECK-DAG:     <<Or:i\d+>>       Or [<<Add1>>,<<Add2>>]
  /// CHECK-DAG:                       Return [<<Or>>]

  /// CHECK-START: int Main.$noinline$AddNegs2(int, int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg1:i\d+>>     Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Neg2:i\d+>>     Neg [<<Arg2>>]
  /// CHECK-DAG:     <<Add1:i\d+>>     Add [<<Neg1>>,<<Neg2>>]
  /// CHECK-DAG:     <<Add2:i\d+>>     Add [<<Neg1>>,<<Neg2>>]
  /// CHECK-NOT:                       Neg
  /// CHECK-DAG:     <<Or:i\d+>>       Or [<<Add1>>,<<Add2>>]
  /// CHECK-DAG:                       Return [<<Or>>]

  /// CHECK-START: int Main.$noinline$AddNegs2(int, int) GVN (after)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg1:i\d+>>     Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Neg2:i\d+>>     Neg [<<Arg2>>]
  /// CHECK-DAG:     <<Add:i\d+>>      Add [<<Neg1>>,<<Neg2>>]
  /// CHECK-DAG:     <<Or:i\d+>>       Or [<<Add>>,<<Add>>]
  /// CHECK-DAG:                       Return [<<Or>>]

  public static int $noinline$AddNegs2(int arg1, int arg2) {
    int temp1 = -arg1;
    int temp2 = -arg2;
    return (temp1 + temp2) | (temp1 + temp2);
  }

  /**
   * This follows test-cases AddNegs1 and AddNegs2.
   * The transformation tested is implemented in
   * `InstructionSimplifierVisitor::TryMoveNegOnInputsAfterBinop`.
   * The optimization should not happen if it moves an additional instruction in
   * the loop.
   */

  /// CHECK-START: long Main.$noinline$AddNegs3(long, long) instruction_simplifier (before)
  //  -------------- Arguments and initial negation operations.
  /// CHECK-DAG:     <<Arg1:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg1:j\d+>>     Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Neg2:j\d+>>     Neg [<<Arg2>>]
  /// CHECK:                           Goto
  //  -------------- Loop
  /// CHECK:                           SuspendCheck
  /// CHECK:         <<Add:j\d+>>      Add [<<Neg1>>,<<Neg2>>]
  /// CHECK:                           Goto

  /// CHECK-START: long Main.$noinline$AddNegs3(long, long) instruction_simplifier (after)
  //  -------------- Arguments and initial negation operations.
  /// CHECK-DAG:     <<Arg1:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg1:j\d+>>     Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Neg2:j\d+>>     Neg [<<Arg2>>]
  /// CHECK:                           Goto
  //  -------------- Loop
  /// CHECK:                           SuspendCheck
  /// CHECK:         <<Add:j\d+>>      Add [<<Neg1>>,<<Neg2>>]
  /// CHECK-NOT:                       Neg
  /// CHECK:                           Goto

  public static long $noinline$AddNegs3(long arg1, long arg2) {
    long res = 0;
    long n_arg1 = -arg1;
    long n_arg2 = -arg2;
    for (long i = 0; i < 1; i++) {
      res += n_arg1 + n_arg2 + i;
    }
    return res;
  }

  /**
   * Test the simplification of an addition with a negated argument into a
   * subtraction.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitAdd`.
   */

  /// CHECK-START: long Main.$noinline$AddNeg1(long, long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg1:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg:j\d+>>      Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Add:j\d+>>      Add [<<Neg>>,<<Arg2>>]
  /// CHECK-DAG:                       Return [<<Add>>]

  /// CHECK-START: long Main.$noinline$AddNeg1(long, long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg1:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Sub:j\d+>>      Sub [<<Arg2>>,<<Arg1>>]
  /// CHECK-DAG:                       Return [<<Sub>>]

  /// CHECK-START: long Main.$noinline$AddNeg1(long, long) instruction_simplifier (after)
  /// CHECK-NOT:                       Neg
  /// CHECK-NOT:                       Add

  public static long $noinline$AddNeg1(long arg1, long arg2) {
    return -arg1 + arg2;
  }

  /**
   * This is similar to the test-case AddNeg1, but the negation has two uses.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitAdd`.
   * The current code won't perform the previous optimization. The
   * transformations do not look at other uses of their inputs. As they don't
   * know what will happen with other uses, they do not take the risk of
   * increasing the register pressure by creating or extending live ranges.
   */

  /// CHECK-START: long Main.$noinline$AddNeg2(long, long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg1:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg:j\d+>>      Neg [<<Arg2>>]
  /// CHECK-DAG:     <<Add1:j\d+>>     Add [<<Arg1>>,<<Neg>>]
  /// CHECK-DAG:     <<Add2:j\d+>>     Add [<<Arg1>>,<<Neg>>]
  /// CHECK-DAG:     <<Res:j\d+>>      Or [<<Add1>>,<<Add2>>]
  /// CHECK-DAG:                       Return [<<Res>>]

  /// CHECK-START: long Main.$noinline$AddNeg2(long, long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg1:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg:j\d+>>      Neg [<<Arg2>>]
  /// CHECK-DAG:     <<Add1:j\d+>>     Add [<<Arg1>>,<<Neg>>]
  /// CHECK-DAG:     <<Add2:j\d+>>     Add [<<Arg1>>,<<Neg>>]
  /// CHECK-DAG:     <<Res:j\d+>>      Or [<<Add1>>,<<Add2>>]
  /// CHECK-DAG:                       Return [<<Res>>]

  /// CHECK-START: long Main.$noinline$AddNeg2(long, long) instruction_simplifier (after)
  /// CHECK-NOT:                       Sub

  public static long $noinline$AddNeg2(long arg1, long arg2) {
    long temp = -arg2;
    return (arg1 + temp) | (arg1 + temp);
  }

  /**
   * Test simplification of the `-(-var)` pattern.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitNeg`.
   */

  /// CHECK-START: long Main.$noinline$NegNeg1(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Neg1:j\d+>>     Neg [<<Arg>>]
  /// CHECK-DAG:     <<Neg2:j\d+>>     Neg [<<Neg1>>]
  /// CHECK-DAG:                       Return [<<Neg2>>]

  /// CHECK-START: long Main.$noinline$NegNeg1(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: long Main.$noinline$NegNeg1(long) instruction_simplifier (after)
  /// CHECK-NOT:                       Neg

  public static long $noinline$NegNeg1(long arg) {
    return -(-arg);
  }

  /**
   * Test 'multi-step' simplification, where a first transformation yields a
   * new simplification possibility for the current instruction.
   * The transformations tested are implemented in `InstructionSimplifierVisitor::VisitNeg`
   * and in `InstructionSimplifierVisitor::VisitAdd`.
   */

  /// CHECK-START: int Main.$noinline$NegNeg2(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Neg1:i\d+>>     Neg [<<Arg>>]
  /// CHECK-DAG:     <<Neg2:i\d+>>     Neg [<<Neg1>>]
  /// CHECK-DAG:     <<Add:i\d+>>      Add [<<Neg2>>,<<Neg1>>]
  /// CHECK-DAG:                       Return [<<Add>>]

  /// CHECK-START: int Main.$noinline$NegNeg2(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Sub:i\d+>>      Sub [<<Arg>>,<<Arg>>]
  /// CHECK-DAG:                       Return [<<Sub>>]

  /// CHECK-START: int Main.$noinline$NegNeg2(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Neg
  /// CHECK-NOT:                       Add

  /// CHECK-START: int Main.$noinline$NegNeg2(int) constant_folding$after_gvn (after)
  /// CHECK:         <<Const0:i\d+>>   IntConstant 0
  /// CHECK-NOT:                       Neg
  /// CHECK-NOT:                       Add
  /// CHECK:                           Return [<<Const0>>]

  public static int $noinline$NegNeg2(int arg) {
    int temp = -arg;
    return temp + -temp;
  }

  /**
   * Test another 'multi-step' simplification, where a first transformation
   * yields a new simplification possibility for the current instruction.
   * The transformations tested are implemented in `InstructionSimplifierVisitor::VisitNeg`
   * and in `InstructionSimplifierVisitor::VisitSub`.
   */

  /// CHECK-START: long Main.$noinline$NegNeg3(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:j\d+>>   LongConstant 0
  /// CHECK-DAG:     <<Neg:j\d+>>      Neg [<<Arg>>]
  /// CHECK-DAG:     <<Sub:j\d+>>      Sub [<<Const0>>,<<Neg>>]
  /// CHECK-DAG:                       Return [<<Sub>>]

  /// CHECK-START: long Main.$noinline$NegNeg3(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: long Main.$noinline$NegNeg3(long) instruction_simplifier (after)
  /// CHECK-NOT:                       Neg
  /// CHECK-NOT:                       Sub

  public static long $noinline$NegNeg3(long arg) {
    return 0 - -arg;
  }

  /**
   * Test that a negated subtraction is simplified to a subtraction with its
   * arguments reversed.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitNeg`.
   */

  /// CHECK-START: int Main.$noinline$NegSub1(int, int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Sub:i\d+>>      Sub [<<Arg1>>,<<Arg2>>]
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Sub>>]
  /// CHECK-DAG:                       Return [<<Neg>>]

  /// CHECK-START: int Main.$noinline$NegSub1(int, int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Sub:i\d+>>      Sub [<<Arg2>>,<<Arg1>>]
  /// CHECK-DAG:                       Return [<<Sub>>]

  /// CHECK-START: int Main.$noinline$NegSub1(int, int) instruction_simplifier (after)
  /// CHECK-NOT:                       Neg

  public static int $noinline$NegSub1(int arg1, int arg2) {
    return -(arg1 - arg2);
  }

  /**
   * This is similar to the test-case NegSub1, but the subtraction has
   * multiple uses.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitNeg`.
   * The current code won't perform the previous optimization. The
   * transformations do not look at other uses of their inputs. As they don't
   * know what will happen with other uses, they do not take the risk of
   * increasing the register pressure by creating or extending live ranges.
   */

  /// CHECK-START: int Main.$noinline$NegSub2(int, int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Sub:i\d+>>      Sub [<<Arg1>>,<<Arg2>>]
  /// CHECK-DAG:     <<Neg1:i\d+>>     Neg [<<Sub>>]
  /// CHECK-DAG:     <<Neg2:i\d+>>     Neg [<<Sub>>]
  /// CHECK-DAG:     <<Or:i\d+>>       Or [<<Neg1>>,<<Neg2>>]
  /// CHECK-DAG:                       Return [<<Or>>]

  /// CHECK-START: int Main.$noinline$NegSub2(int, int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Sub:i\d+>>      Sub [<<Arg1>>,<<Arg2>>]
  /// CHECK-DAG:     <<Neg1:i\d+>>     Neg [<<Sub>>]
  /// CHECK-DAG:     <<Neg2:i\d+>>     Neg [<<Sub>>]
  /// CHECK-DAG:     <<Or:i\d+>>       Or [<<Neg1>>,<<Neg2>>]
  /// CHECK-DAG:                       Return [<<Or>>]

  public static int $noinline$NegSub2(int arg1, int arg2) {
    int temp = arg1 - arg2;
    return -temp | -temp;
  }

    /**
   * Test the simplification of a subtraction with a negated argument.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitSub`.
   */

  /// CHECK-START: int Main.$noinline$SubNeg1(int, int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Sub:i\d+>>      Sub [<<Neg>>,<<Arg2>>]
  /// CHECK-DAG:                       Return [<<Sub>>]

  /// CHECK-START: int Main.$noinline$SubNeg1(int, int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Add:i\d+>>      Add [<<Arg1>>,<<Arg2>>]
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Add>>]
  /// CHECK-DAG:                       Return [<<Neg>>]

  /// CHECK-START: int Main.$noinline$SubNeg1(int, int) instruction_simplifier (after)
  /// CHECK-NOT:                       Sub

  public static int $noinline$SubNeg1(int arg1, int arg2) {
    return -arg1 - arg2;
  }

  /**
   * This is similar to the test-case SubNeg1, but the negation has
   * multiple uses.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitSub`.
   * The current code won't perform the previous optimization. The
   * transformations do not look at other uses of their inputs. As they don't
   * know what will happen with other uses, they do not take the risk of
   * increasing the register pressure by creating or extending live ranges.
   */

  /// CHECK-START: int Main.$noinline$SubNeg2(int, int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Sub1:i\d+>>     Sub [<<Neg>>,<<Arg2>>]
  /// CHECK-DAG:     <<Sub2:i\d+>>     Sub [<<Neg>>,<<Arg2>>]
  /// CHECK-DAG:     <<Or:i\d+>>       Or [<<Sub1>>,<<Sub2>>]
  /// CHECK-DAG:                       Return [<<Or>>]

  /// CHECK-START: int Main.$noinline$SubNeg2(int, int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Sub1:i\d+>>     Sub [<<Neg>>,<<Arg2>>]
  /// CHECK-DAG:     <<Sub2:i\d+>>     Sub [<<Neg>>,<<Arg2>>]
  /// CHECK-DAG:     <<Or:i\d+>>       Or [<<Sub1>>,<<Sub2>>]
  /// CHECK-DAG:                       Return [<<Or>>]

  /// CHECK-START: int Main.$noinline$SubNeg2(int, int) instruction_simplifier (after)
  /// CHECK-NOT:                       Add

  public static int $noinline$SubNeg2(int arg1, int arg2) {
    int temp = -arg1;
    return (temp - arg2) | (temp - arg2);
  }

  /**
   * This follows test-cases SubNeg1 and SubNeg2.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitSub`.
   * The optimization should not happen if it moves an additional instruction in
   * the loop.
   */

  /// CHECK-START: long Main.$noinline$SubNeg3(long, long) instruction_simplifier (before)
  //  -------------- Arguments and initial negation operation.
  /// CHECK-DAG:     <<Arg1:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg:j\d+>>      Neg [<<Arg1>>]
  /// CHECK:                           Goto
  //  -------------- Loop
  /// CHECK:                           SuspendCheck
  /// CHECK:         <<Sub:j\d+>>      Sub [<<Neg>>,<<Arg2>>]
  /// CHECK:                           Goto

  /// CHECK-START: long Main.$noinline$SubNeg3(long, long) instruction_simplifier (after)
  //  -------------- Arguments and initial negation operation.
  /// CHECK-DAG:     <<Arg1:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg:j\d+>>      Neg [<<Arg1>>]
  /// CHECK-DAG:                       Goto
  //  -------------- Loop
  /// CHECK:                           SuspendCheck
  /// CHECK:         <<Sub:j\d+>>      Sub [<<Neg>>,<<Arg2>>]
  /// CHECK-NOT:                       Neg
  /// CHECK:                           Goto

  public static long $noinline$SubNeg3(long arg1, long arg2) {
    long res = 0;
    long temp = -arg1;
    for (long i = 0; i < 1; i++) {
      res += temp - arg2 - i;
    }
    return res;
  }

  /// CHECK-START: boolean Main.$noinline$EqualBoolVsIntConst(boolean) dead_code_elimination$after_inlining (before)
  /// CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
  /// CHECK-DAG:     <<Const2:i\d+>>   IntConstant 2
  /// CHECK-DAG:                       If [<<Arg>>]
  /// CHECK-DAG:     <<Phi1:i\d+>>     Phi [<<Const0>>,<<Const1>>]
  /// CHECK-DAG:     <<Cond:z\d+>>     Equal [<<Phi1>>,<<Const2>>]
  /// CHECK-DAG:                       If [<<Cond>>]
  /// CHECK-DAG:     <<Phi2:i\d+>>     Phi [<<Const0>>,<<Const1>>]
  /// CHECK-DAG:                       Return [<<Phi2>>]

  /// CHECK-START: boolean Main.$noinline$EqualBoolVsIntConst(boolean) dead_code_elimination$after_inlining (after)
  /// CHECK-DAG:     <<True:i\d+>>     IntConstant 1
  /// CHECK-DAG:                       Return [<<True>>]

  public static boolean $noinline$EqualBoolVsIntConst(boolean arg) {
    // Make calls that will be inlined to make sure the instruction simplifier
    // sees the simplification (dead code elimination will also try to simplify it).
    return (arg ? $inline$ReturnArg(0) : $inline$ReturnArg(1)) != 2;
  }

  public static int $inline$ReturnArg(int arg) {
    return arg;
  }

  /// CHECK-START: boolean Main.$noinline$NotEqualBoolVsIntConst(boolean) instruction_simplifier$after_inlining (before)
  /// CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
  /// CHECK-DAG:     <<Const2:i\d+>>   IntConstant 2
  /// CHECK-DAG:                       If [<<Arg>>]
  /// CHECK-DAG:     <<Phi1:i\d+>>     Phi [<<Const0>>,<<Const1>>]
  /// CHECK-DAG:     <<Cond:z\d+>>     NotEqual [<<Phi1>>,<<Const2>>]
  /// CHECK-DAG:                       If [<<Cond>>]
  /// CHECK-DAG:     <<Phi2:i\d+>>     Phi [<<Const0>>,<<Const1>>]
  /// CHECK-DAG:                       Return [<<Phi2>>]

  /// CHECK-START: boolean Main.$noinline$NotEqualBoolVsIntConst(boolean) dead_code_elimination$after_inlining (after)
  /// CHECK-DAG:     <<False:i\d+>>    IntConstant 0
  /// CHECK-DAG:                       Return [<<False>>]

  public static boolean $noinline$NotEqualBoolVsIntConst(boolean arg) {
    // Make calls that will be inlined to make sure the instruction simplifier
    // sees the simplification (dead code elimination will also try to simplify it).
    return (arg ? $inline$ReturnArg(0) : $inline$ReturnArg(1)) == 2;
  }

  /// CHECK-START: float Main.$noinline$Div2(float) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const2:f\d+>>   FloatConstant 2
  /// CHECK-DAG:      <<Div:f\d+>>      Div [<<Arg>>,<<Const2>>]
  /// CHECK-DAG:                        Return [<<Div>>]

  /// CHECK-START: float Main.$noinline$Div2(float) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<ConstP5:f\d+>>  FloatConstant 0.5
  /// CHECK-DAG:      <<Mul:f\d+>>      Mul [<<Arg>>,<<ConstP5>>]
  /// CHECK-DAG:                        Return [<<Mul>>]

  /// CHECK-START: float Main.$noinline$Div2(float) instruction_simplifier (after)
  /// CHECK-NOT:                        Div

  public static float $noinline$Div2(float arg) {
    return arg / 2.0f;
  }

  /// CHECK-START: double Main.$noinline$Div2(double) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:d\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const2:d\d+>>   DoubleConstant 2
  /// CHECK-DAG:      <<Div:d\d+>>      Div [<<Arg>>,<<Const2>>]
  /// CHECK-DAG:                        Return [<<Div>>]

  /// CHECK-START: double Main.$noinline$Div2(double) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:d\d+>>      ParameterValue
  /// CHECK-DAG:      <<ConstP5:d\d+>>  DoubleConstant 0.5
  /// CHECK-DAG:      <<Mul:d\d+>>      Mul [<<Arg>>,<<ConstP5>>]
  /// CHECK-DAG:                        Return [<<Mul>>]

  /// CHECK-START: double Main.$noinline$Div2(double) instruction_simplifier (after)
  /// CHECK-NOT:                        Div
  public static double $noinline$Div2(double arg) {
    return arg / 2.0;
  }

  /// CHECK-START: float Main.$noinline$DivMP25(float) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<ConstMP25:f\d+>>   FloatConstant -0.25
  /// CHECK-DAG:      <<Div:f\d+>>      Div [<<Arg>>,<<ConstMP25>>]
  /// CHECK-DAG:                        Return [<<Div>>]

  /// CHECK-START: float Main.$noinline$DivMP25(float) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<ConstM4:f\d+>>  FloatConstant -4
  /// CHECK-DAG:      <<Mul:f\d+>>      Mul [<<Arg>>,<<ConstM4>>]
  /// CHECK-DAG:                        Return [<<Mul>>]

  /// CHECK-START: float Main.$noinline$DivMP25(float) instruction_simplifier (after)
  /// CHECK-NOT:                        Div

  public static float $noinline$DivMP25(float arg) {
    return arg / -0.25f;
  }

  /// CHECK-START: double Main.$noinline$DivMP25(double) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:d\d+>>      ParameterValue
  /// CHECK-DAG:      <<ConstMP25:d\d+>>   DoubleConstant -0.25
  /// CHECK-DAG:      <<Div:d\d+>>      Div [<<Arg>>,<<ConstMP25>>]
  /// CHECK-DAG:                        Return [<<Div>>]

  /// CHECK-START: double Main.$noinline$DivMP25(double) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:d\d+>>      ParameterValue
  /// CHECK-DAG:      <<ConstM4:d\d+>>  DoubleConstant -4
  /// CHECK-DAG:      <<Mul:d\d+>>      Mul [<<Arg>>,<<ConstM4>>]
  /// CHECK-DAG:                        Return [<<Mul>>]

  /// CHECK-START: double Main.$noinline$DivMP25(double) instruction_simplifier (after)
  /// CHECK-NOT:                        Div
  public static double $noinline$DivMP25(double arg) {
    return arg / -0.25f;
  }

  /**
   * Test strength reduction of factors of the form (2^n + 1).
   */

  /// CHECK-START: int Main.$noinline$mulPow2Plus1(int) instruction_simplifier (before)
  /// CHECK-DAG:   <<Arg:i\d+>>         ParameterValue
  /// CHECK-DAG:   <<Const9:i\d+>>      IntConstant 9
  /// CHECK:                            Mul [<<Arg>>,<<Const9>>]

  /// CHECK-START: int Main.$noinline$mulPow2Plus1(int) instruction_simplifier (after)
  /// CHECK-DAG:   <<Arg:i\d+>>         ParameterValue
  /// CHECK-DAG:   <<Const3:i\d+>>      IntConstant 3
  /// CHECK:       <<Shift:i\d+>>       Shl [<<Arg>>,<<Const3>>]
  /// CHECK-NEXT:                       Add [<<Arg>>,<<Shift>>]

  public static int $noinline$mulPow2Plus1(int arg) {
    return arg * 9;
  }

  /**
   * Test strength reduction of factors of the form (2^n - 1).
   */

  /// CHECK-START: long Main.$noinline$mulPow2Minus1(long) instruction_simplifier (before)
  /// CHECK-DAG:   <<Arg:j\d+>>         ParameterValue
  /// CHECK-DAG:   <<Const31:j\d+>>     LongConstant 31
  /// CHECK:                            Mul [<<Const31>>,<<Arg>>]

  /// CHECK-START: long Main.$noinline$mulPow2Minus1(long) instruction_simplifier (after)
  /// CHECK-DAG:   <<Arg:j\d+>>         ParameterValue
  /// CHECK-DAG:   <<Const5:i\d+>>      IntConstant 5
  /// CHECK:       <<Shift:j\d+>>       Shl [<<Arg>>,<<Const5>>]
  /// CHECK-NEXT:                       Sub [<<Shift>>,<<Arg>>]

  public static long $noinline$mulPow2Minus1(long arg) {
    return arg * 31;
  }

  /// CHECK-START: int Main.$noinline$booleanFieldNotEqualOne() instruction_simplifier$after_inlining (before)
  /// CHECK-DAG:      <<Const1:i\d+>>   IntConstant 1
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<Field:z\d+>>    StaticFieldGet
  /// CHECK-DAG:      <<NE:z\d+>>       NotEqual [<<Field>>,<<Const1>>]
  /// CHECK-DAG:                        If [<<NE>>]
  /// CHECK-DAG:      <<Phi:i\d+>>      Phi [<<Const13>>,<<Const54>>]
  /// CHECK-DAG:                        Return [<<Phi>>]

  /// CHECK-START: int Main.$noinline$booleanFieldNotEqualOne() control_flow_simplifier (after)
  /// CHECK-DAG:      <<Field:z\d+>>    StaticFieldGet
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<Select:i\d+>>   Select [<<Const54>>,<<Const13>>,<<Field>>]
  /// CHECK-DAG:                        Return [<<Select>>]

  public static int $noinline$booleanFieldNotEqualOne() {
    return (booleanField == $inline$true()) ? 13 : 54;
  }

  /// CHECK-START: int Main.$noinline$booleanFieldEqualZero() instruction_simplifier$after_inlining (before)
  /// CHECK-DAG:      <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<Field:z\d+>>    StaticFieldGet
  /// CHECK-DAG:      <<EQ:z\d+>>       Equal [<<Field>>,<<Const0>>]
  /// CHECK-DAG:                        If [<<EQ>>]
  /// CHECK-DAG:      <<Phi:i\d+>>      Phi [<<Const13>>,<<Const54>>]
  /// CHECK-DAG:                        Return [<<Phi>>]

  /// CHECK-START: int Main.$noinline$booleanFieldEqualZero() control_flow_simplifier (after)
  /// CHECK-DAG:      <<Field:z\d+>>    StaticFieldGet
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<Select:i\d+>>   Select [<<Const54>>,<<Const13>>,<<Field>>]
  /// CHECK-DAG:                        Return [<<Select>>]

  public static int $noinline$booleanFieldEqualZero() {
    return (booleanField != $inline$false()) ? 13 : 54;
  }

  /// CHECK-START: int Main.$noinline$intConditionNotEqualOne(int) instruction_simplifier$after_inlining (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:      <<Const1:i\d+>>   IntConstant 1
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const42:i\d+>>  IntConstant 42
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<LE:z\d+>>       LessThanOrEqual [<<Arg>>,<<Const42>>]
  /// CHECK-DAG:                        If [<<LE>>]
  /// CHECK-DAG:      <<Phi1:i\d+>>     Phi [<<Const1>>,<<Const0>>]
  /// CHECK-DAG:      <<NE:z\d+>>       NotEqual [<<Phi1>>,<<Const1>>]
  /// CHECK-DAG:                        If [<<NE>>]
  /// CHECK-DAG:      <<Phi2:i\d+>>     Phi [<<Const13>>,<<Const54>>]
  /// CHECK-DAG:                        Return [<<Phi2>>]

  /// CHECK-START: int Main.$noinline$intConditionNotEqualOne(int) control_flow_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const42:i\d+>>  IntConstant 42
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<LE:z\d+>>       LessThanOrEqual [<<Arg>>,<<Const42>>]
  /// CHECK-DAG:      <<Result:i\d+>>   Select [<<Const13>>,<<Const54>>,<<LE>>]
  /// CHECK-DAG:                        Return [<<Result>>]
  // Note that we match `LE` from Select because there are two identical
  // LessThanOrEqual instructions.

  public static int $noinline$intConditionNotEqualOne(int i) {
    return ((i > 42) == $inline$true()) ? 13 : 54;
  }

  /// CHECK-START: int Main.$noinline$intConditionEqualZero(int) instruction_simplifier$after_inlining (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:      <<Const1:i\d+>>   IntConstant 1
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const42:i\d+>>  IntConstant 42
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<LE:z\d+>>       LessThanOrEqual [<<Arg>>,<<Const42>>]
  /// CHECK-DAG:                        If [<<LE>>]
  /// CHECK-DAG:      <<Phi1:i\d+>>     Phi [<<Const1>>,<<Const0>>]
  /// CHECK-DAG:      <<EQ:z\d+>>       Equal [<<Phi1>>,<<Const0>>]
  /// CHECK-DAG:                        If [<<EQ>>]
  /// CHECK-DAG:      <<Phi2:i\d+>>     Phi [<<Const13>>,<<Const54>>]
  /// CHECK-DAG:                        Return [<<Phi2>>]

  /// CHECK-START: int Main.$noinline$intConditionEqualZero(int) control_flow_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const42:i\d+>>  IntConstant 42
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<LE:z\d+>>       LessThanOrEqual [<<Arg>>,<<Const42>>]
  /// CHECK-DAG:      <<Result:i\d+>>   Select [<<Const13>>,<<Const54>>,<<LE>>]
  /// CHECK-DAG:                        Return [<<Result>>]
  // Note that we match `LE` from Select because there are two identical
  // LessThanOrEqual instructions.

  public static int $noinline$intConditionEqualZero(int i) {
    return ((i > 42) != $inline$false()) ? 13 : 54;
  }

  // Test that conditions on float/double are not flipped.

  /// CHECK-START: int Main.$noinline$floatConditionNotEqualOne(float) builder (after)
  /// CHECK:                            LessThanOrEqual

  /// CHECK-START: int Main.$noinline$floatConditionNotEqualOne(float) instruction_simplifier$before_codegen (after)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<Const42:f\d+>>  FloatConstant 42
  /// CHECK-DAG:      <<LE:z\d+>>       LessThanOrEqual [<<Arg>>,<<Const42>>]
  /// CHECK-DAG:      <<Select:i\d+>>   Select [<<Const13>>,<<Const54>>,<<LE>>]
  /// CHECK-DAG:                        Return [<<Select>>]

  public static int $noinline$floatConditionNotEqualOne(float f) {
    return ((f > 42.0f) == true) ? 13 : 54;
  }

  /// CHECK-START: int Main.$noinline$doubleConditionEqualZero(double) builder (after)
  /// CHECK:                            LessThanOrEqual

  /// CHECK-START: int Main.$noinline$doubleConditionEqualZero(double) instruction_simplifier$before_codegen (after)
  /// CHECK-DAG:      <<Arg:d\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<Const42:d\d+>>  DoubleConstant 42
  /// CHECK-DAG:      <<LE:z\d+>>       LessThanOrEqual [<<Arg>>,<<Const42>>]
  /// CHECK-DAG:      <<Select:i\d+>>   Select [<<Const13>>,<<Const54>>,<<LE>>]
  /// CHECK-DAG:                        Return [<<Select>>]

  public static int $noinline$doubleConditionEqualZero(double d) {
    return ((d > 42.0) != false) ? 13 : 54;
  }

  /// CHECK-START: int Main.$noinline$intToDoubleToInt(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Double>>]
  /// CHECK-DAG:                        Return [<<Int>>]

  /// CHECK-START: int Main.$noinline$intToDoubleToInt(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:                        Return [<<Arg>>]

  /// CHECK-START: int Main.$noinline$intToDoubleToInt(int) instruction_simplifier (after)
  /// CHECK-NOT:                        TypeConversion

  public static int $noinline$intToDoubleToInt(int value) {
    // Lossless conversion followed by a conversion back.
    return (int) (double) value;
  }

  /// CHECK-START: java.lang.String Main.$noinline$intToDoubleToIntPrint(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:      {{i\d+}}          TypeConversion [<<Double>>]

  /// CHECK-START: java.lang.String Main.$noinline$intToDoubleToIntPrint(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      {{d\d+}}          TypeConversion [<<Arg>>]

  /// CHECK-START: java.lang.String Main.$noinline$intToDoubleToIntPrint(int) instruction_simplifier (after)
  /// CHECK-DAG:                        TypeConversion
  /// CHECK-NOT:                        TypeConversion

  public static String $noinline$intToDoubleToIntPrint(int value) {
    // Lossless conversion followed by a conversion back
    // with another use of the intermediate result.
    double d = (double) value;
    int i = (int) d;
    return "d=" + d + ", i=" + i;
  }

  /// CHECK-START: int Main.$noinline$byteToDoubleToInt(byte) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:b\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Double>>]
  /// CHECK-DAG:                        Return [<<Int>>]

  /// CHECK-START: int Main.$noinline$byteToDoubleToInt(byte) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:b\d+>>      ParameterValue
  /// CHECK-DAG:                        Return [<<Arg>>]

  /// CHECK-START: int Main.$noinline$byteToDoubleToInt(byte) instruction_simplifier (after)
  /// CHECK-NOT:                        TypeConversion

  public static int $noinline$byteToDoubleToInt(byte value) {
    // Lossless conversion followed by another conversion, use implicit conversion.
    return (int) (double) value;
  }

  /// CHECK-START: int Main.$noinline$floatToDoubleToInt(float) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Double>>]
  /// CHECK-DAG:                        Return [<<Int>>]

  /// CHECK-START: int Main.$noinline$floatToDoubleToInt(float) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Arg>>]
  /// CHECK-DAG:                        Return [<<Int>>]

  /// CHECK-START: int Main.$noinline$floatToDoubleToInt(float) instruction_simplifier (after)
  /// CHECK-DAG:                        TypeConversion
  /// CHECK-NOT:                        TypeConversion

  public static int $noinline$floatToDoubleToInt(float value) {
    // Lossless conversion followed by another conversion.
    return (int) (double) value;
  }

  /// CHECK-START: java.lang.String Main.$noinline$floatToDoubleToIntPrint(float) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:      {{i\d+}}          TypeConversion [<<Double>>]

  /// CHECK-START: java.lang.String Main.$noinline$floatToDoubleToIntPrint(float) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:      {{i\d+}}          TypeConversion [<<Double>>]

  public static String $noinline$floatToDoubleToIntPrint(float value) {
    // Lossless conversion followed by another conversion with
    // an extra use of the intermediate result.
    double d = (double) value;
    int i = (int) d;
    return "d=" + d + ", i=" + i;
  }

  /// CHECK-START: short Main.$noinline$byteToDoubleToShort(byte) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:b\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Double>>]
  /// CHECK-DAG:      <<Short:s\d+>>    TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Short>>]

  /// CHECK-START: short Main.$noinline$byteToDoubleToShort(byte) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:b\d+>>      ParameterValue
  /// CHECK-DAG:                        Return [<<Arg>>]

  /// CHECK-START: short Main.$noinline$byteToDoubleToShort(byte) instruction_simplifier (after)
  /// CHECK-NOT:                        TypeConversion

  public static short $noinline$byteToDoubleToShort(byte value) {
    // Originally, this is byte->double->int->short. The first conversion is lossless,
    // so we merge this with the second one to byte->int which we omit as it's an implicit
    // conversion. Then we eliminate the resulting byte->short as an implicit conversion.
    return (short) (double) value;
  }

  /// CHECK-START: short Main.$noinline$charToDoubleToShort(char) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:c\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Double>>]
  /// CHECK-DAG:      <<Short:s\d+>>    TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Short>>]

  /// CHECK-START: short Main.$noinline$charToDoubleToShort(char) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:c\d+>>      ParameterValue
  /// CHECK-DAG:      <<Short:s\d+>>    TypeConversion [<<Arg>>]
  /// CHECK-DAG:                        Return [<<Short>>]

  /// CHECK-START: short Main.$noinline$charToDoubleToShort(char) instruction_simplifier (after)
  /// CHECK-DAG:                        TypeConversion
  /// CHECK-NOT:                        TypeConversion

  public static short $noinline$charToDoubleToShort(char value) {
    // Originally, this is char->double->int->short. The first conversion is lossless,
    // so we merge this with the second one to char->int which we omit as it's an implicit
    // conversion. Then we are left with the resulting char->short conversion.
    return (short) (double) value;
  }

  /// CHECK-START: short Main.$noinline$floatToIntToShort(float) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Short:s\d+>>    TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Short>>]

  /// CHECK-START: short Main.$noinline$floatToIntToShort(float) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Short:s\d+>>    TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Short>>]

  public static short $noinline$floatToIntToShort(float value) {
    // Lossy FP to integral conversion followed by another conversion: no simplification.
    return (short) value;
  }

  /// CHECK-START: int Main.$noinline$intToFloatToInt(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Float:f\d+>>    TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Float>>]
  /// CHECK-DAG:                        Return [<<Int>>]

  /// CHECK-START: int Main.$noinline$intToFloatToInt(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Float:f\d+>>    TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Float>>]
  /// CHECK-DAG:                        Return [<<Int>>]

  public static int $noinline$intToFloatToInt(int value) {
    // Lossy integral to FP conversion followed another conversion: no simplification.
    return (int) (float) value;
  }

  /// CHECK-START: double Main.$noinline$longToIntToDouble(long) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Double>>]

  /// CHECK-START: double Main.$noinline$longToIntToDouble(long) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Double>>]

  public static double $noinline$longToIntToDouble(long value) {
    // Lossy long-to-int conversion followed an integral to FP conversion: no simplification.
    return (double) (int) value;
  }

  /// CHECK-START: long Main.$noinline$longToIntToLong(long) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Long:j\d+>>     TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Long>>]

  /// CHECK-START: long Main.$noinline$longToIntToLong(long) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Long:j\d+>>     TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Long>>]

  public static long $noinline$longToIntToLong(long value) {
    // Lossy long-to-int conversion followed an int-to-long conversion: no simplification.
    return (long) (int) value;
  }

  /// CHECK-START: short Main.$noinline$shortToCharToShort(short) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:      <<Char:c\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Short:s\d+>>    TypeConversion [<<Char>>]
  /// CHECK-DAG:                        Return [<<Short>>]

  /// CHECK-START: short Main.$noinline$shortToCharToShort(short) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:                        Return [<<Arg>>]

  public static short $noinline$shortToCharToShort(short value) {
    // Integral conversion followed by non-widening integral conversion to original type.
    return (short) (char) value;
  }

  /// CHECK-START: int Main.$noinline$shortToLongToInt(short) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:      <<Long:j\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Long>>]
  /// CHECK-DAG:                        Return [<<Int>>]

  /// CHECK-START: int Main.$noinline$shortToLongToInt(short) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:                        Return [<<Arg>>]

  public static int $noinline$shortToLongToInt(short value) {
    // Integral conversion followed by non-widening integral conversion, use implicit conversion.
    return (int) (long) value;
  }

  /// CHECK-START: byte Main.$noinline$shortToCharToByte(short) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:      <<Char:c\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Byte:b\d+>>     TypeConversion [<<Char>>]
  /// CHECK-DAG:                        Return [<<Byte>>]

  /// CHECK-START: byte Main.$noinline$shortToCharToByte(short) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:      <<Byte:b\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:                        Return [<<Byte>>]

  public static byte $noinline$shortToCharToByte(short value) {
    // Integral conversion followed by non-widening integral conversion losing bits
    // from the original type. Simplify to use only one conversion.
    return (byte) (char) value;
  }

  /// CHECK-START: java.lang.String Main.$noinline$shortToCharToBytePrint(short) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:      <<Char:c\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:      {{b\d+}}          TypeConversion [<<Char>>]

  /// CHECK-START: java.lang.String Main.$noinline$shortToCharToBytePrint(short) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:      <<Char:c\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:      {{b\d+}}          TypeConversion [<<Char>>]

  public static String $noinline$shortToCharToBytePrint(short value) {
    // Integral conversion followed by non-widening integral conversion losing bits
    // from the original type with an extra use of the intermediate result.
    char c = (char) value;
    byte b = (byte) c;
    return "c=" + ((int) c) + ", b=" + ((int) b);  // implicit conversions.
  }

  /// CHECK-START: long Main.$noinline$intAndSmallLongConstant(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Mask:j\d+>>     LongConstant -12345678
  /// CHECK-DAG:      <<Long:j\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<And:j\d+>>      And [<<Long>>,<<Mask>>]
  /// CHECK-DAG:                        Return [<<And>>]

  /// CHECK-START: long Main.$noinline$intAndSmallLongConstant(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Mask:i\d+>>     IntConstant -12345678
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Arg>>,<<Mask>>]
  /// CHECK-DAG:      <<Long:j\d+>>     TypeConversion [<<And>>]
  /// CHECK-DAG:                        Return [<<Long>>]

  public static long $noinline$intAndSmallLongConstant(int value) {
    return value & -12345678L;  // Shall be simplified (constant is 32-bit).
  }

  /// CHECK-START: long Main.$noinline$intAndLargeLongConstant(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Mask:j\d+>>     LongConstant 9876543210
  /// CHECK-DAG:      <<Long:j\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<And:j\d+>>      And [<<Long>>,<<Mask>>]
  /// CHECK-DAG:                        Return [<<And>>]

  /// CHECK-START: long Main.$noinline$intAndLargeLongConstant(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Mask:j\d+>>     LongConstant 9876543210
  /// CHECK-DAG:      <<Long:j\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<And:j\d+>>      And [<<Long>>,<<Mask>>]
  /// CHECK-DAG:                        Return [<<And>>]

  public static long $noinline$intAndLargeLongConstant(int value) {
    return value & 9876543210L;  // Shall not be simplified (constant is not 32-bit).
  }

  /// CHECK-START: long Main.$noinline$intShr28And15L(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Shift:i\d+>>    IntConstant 28
  /// CHECK-DAG:      <<Mask:j\d+>>     LongConstant 15
  /// CHECK-DAG:      <<Shifted:i\d+>>  Shr [<<Arg>>,<<Shift>>]
  /// CHECK-DAG:      <<Long:j\d+>>     TypeConversion [<<Shifted>>]
  /// CHECK-DAG:      <<And:j\d+>>      And [<<Long>>,<<Mask>>]
  /// CHECK-DAG:                        Return [<<And>>]

  /// CHECK-START: long Main.$noinline$intShr28And15L(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Shift:i\d+>>    IntConstant 28
  /// CHECK-DAG:      <<Shifted:i\d+>>  UShr [<<Arg>>,<<Shift>>]
  /// CHECK-DAG:      <<Long:j\d+>>     TypeConversion [<<Shifted>>]
  /// CHECK-DAG:                        Return [<<Long>>]

  public static long $noinline$intShr28And15L(int value) {
    return (value >> 28) & 15L;
  }

  /// CHECK-START: byte Main.$noinline$longAnd0xffToByte(long) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:      <<Mask:j\d+>>     LongConstant 255
  /// CHECK-DAG:      <<And:j\d+>>      And [<<Mask>>,<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<And>>]
  /// CHECK-DAG:      <<Byte:b\d+>>     TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Byte>>]

  /// CHECK-START: byte Main.$noinline$longAnd0xffToByte(long) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:      <<Byte:b\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:                        Return [<<Byte>>]

  /// CHECK-START: byte Main.$noinline$longAnd0xffToByte(long) instruction_simplifier (after)
  /// CHECK-NOT:                        And

  public static byte $noinline$longAnd0xffToByte(long value) {
    return (byte) (value & 0xff);
  }

  /// CHECK-START: char Main.$noinline$intAnd0x1ffffToChar(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Mask:i\d+>>     IntConstant 131071
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Mask>>,<<Arg>>]
  /// CHECK-DAG:      <<Char:c\d+>>     TypeConversion [<<And>>]
  /// CHECK-DAG:                        Return [<<Char>>]

  /// CHECK-START: char Main.$noinline$intAnd0x1ffffToChar(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Char:c\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:                        Return [<<Char>>]

  /// CHECK-START: char Main.$noinline$intAnd0x1ffffToChar(int) instruction_simplifier (after)
  /// CHECK-NOT:                        And

  public static char $noinline$intAnd0x1ffffToChar(int value) {
    // Keeping all significant bits and one more.
    return (char) (value & 0x1ffff);
  }

  /// CHECK-START: short Main.$noinline$intAnd0x17fffToShort(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Mask:i\d+>>     IntConstant 98303
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Mask>>,<<Arg>>]
  /// CHECK-DAG:      <<Short:s\d+>>    TypeConversion [<<And>>]
  /// CHECK-DAG:                        Return [<<Short>>]

  /// CHECK-START: short Main.$noinline$intAnd0x17fffToShort(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Mask:i\d+>>     IntConstant 98303
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Mask>>,<<Arg>>]
  /// CHECK-DAG:      <<Short:s\d+>>    TypeConversion [<<And>>]
  /// CHECK-DAG:                        Return [<<Short>>]

  public static short $noinline$intAnd0x17fffToShort(int value) {
    // No simplification: clearing a significant bit.
    return (short) (value & 0x17fff);
  }

  /// CHECK-START: double Main.$noinline$shortAnd0xffffToShortToDouble(short) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:      <<Mask:i\d+>>     IntConstant 65535
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Mask>>,<<Arg>>]
  /// CHECK-DAG:      <<Same:s\d+>>     TypeConversion [<<And>>]
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Same>>]
  /// CHECK-DAG:                        Return [<<Double>>]

  /// CHECK-START: double Main.$noinline$shortAnd0xffffToShortToDouble(short) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:                        Return [<<Double>>]

  public static double $noinline$shortAnd0xffffToShortToDouble(short value) {
    short same = (short) (value & 0xffff);
    return (double) same;
  }

  /// CHECK-START: int Main.$noinline$intReverseCondition(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const42:i\d+>>  IntConstant 42
  /// CHECK-DAG:      <<LE:z\d+>>       LessThanOrEqual [<<Const42>>,<<Arg>>]

  /// CHECK-START: int Main.$noinline$intReverseCondition(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const42:i\d+>>  IntConstant 42
  /// CHECK-DAG:      <<GE:z\d+>>       GreaterThanOrEqual [<<Arg>>,<<Const42>>]

  public static int $noinline$intReverseCondition(int i) {
    return (42 > i) ? 13 : 54;
  }

  /// CHECK-START: int Main.$noinline$intReverseConditionNaN(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Const42:d\d+>>  DoubleConstant 42
  /// CHECK-DAG:      <<Result:d\d+>>   InvokeStaticOrDirect
  /// CHECK-DAG:      <<CMP:i\d+>>      Compare [<<Const42>>,<<Result>>]

  /// CHECK-START: int Main.$noinline$intReverseConditionNaN(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Const42:d\d+>>  DoubleConstant 42
  /// CHECK-DAG:      <<Result:d\d+>>   InvokeStaticOrDirect
  /// CHECK-DAG:      <<EQ:z\d+>>       Equal [<<Result>>,<<Const42>>]

  public static int $noinline$intReverseConditionNaN(int i) {
    return (42 != Math.sqrt(i)) ? 13 : 54;
  }

  public static int $noinline$runSmaliTest(String name, boolean input) {
    try {
      Class<?> c = Class.forName("SmaliTests");
      Method m = c.getMethod(name, boolean.class);
      return (Integer) m.invoke(null, input);
    } catch (Exception ex) {
      throw new Error(ex);
    }
  }

  public static boolean $noinline$runSmaliTest2Boolean(String name, boolean input) {
    try {
      Class<?> c = Class.forName("SmaliTests2");
      Method m = c.getMethod(name, boolean.class);
      return (Boolean) m.invoke(null, input);
    } catch (Exception ex) {
      throw new Error(ex);
    }
  }

  public static int $noinline$runSmaliTestInt(String postfix, String name, int arg) {
    try {
      Class<?> c = Class.forName("SmaliTests" + postfix);
      Method m = c.getMethod(name, int.class);
      return (Integer) m.invoke(null, arg);
    } catch (Exception ex) {
      throw new Error(ex);
    }
  }

  public static int $noinline$runSmaliTestInt(String name, int arg) {
    return $noinline$runSmaliTestInt("", name, arg);
  }

  public static long $noinline$runSmaliTest2Long(String name, long arg) {
    try {
      Class<?> c = Class.forName("SmaliTests2");
      Method m = c.getMethod(name, long.class);
      return (Long) m.invoke(null, arg);
    } catch (Exception ex) {
      throw new Error(ex);
    }
  }

  /// CHECK-START: int Main.$noinline$intUnnecessaryShiftMasking(int, int) instruction_simplifier (before)
  /// CHECK:          <<Value:i\d+>>    ParameterValue
  /// CHECK:          <<Shift:i\d+>>    ParameterValue
  /// CHECK-DAG:      <<Const31:i\d+>>  IntConstant 31
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Shift>>,<<Const31>>]
  /// CHECK-DAG:      <<Shl:i\d+>>      Shl [<<Value>>,<<And>>]
  /// CHECK-DAG:                        Return [<<Shl>>]

  /// CHECK-START: int Main.$noinline$intUnnecessaryShiftMasking(int, int) instruction_simplifier (after)
  /// CHECK:          <<Value:i\d+>>    ParameterValue
  /// CHECK:          <<Shift:i\d+>>    ParameterValue
  /// CHECK-DAG:      <<Shl:i\d+>>      Shl [<<Value>>,<<Shift>>]
  /// CHECK-DAG:                        Return [<<Shl>>]

  public static int $noinline$intUnnecessaryShiftMasking(int value, int shift) {
    return value << (shift & 31);
  }

  /// CHECK-START: long Main.$noinline$longUnnecessaryShiftMasking(long, int) instruction_simplifier (before)
  /// CHECK:          <<Value:j\d+>>    ParameterValue
  /// CHECK:          <<Shift:i\d+>>    ParameterValue
  /// CHECK-DAG:      <<Const63:i\d+>>  IntConstant 63
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Shift>>,<<Const63>>]
  /// CHECK-DAG:      <<Shr:j\d+>>      Shr [<<Value>>,<<And>>]
  /// CHECK-DAG:                        Return [<<Shr>>]

  /// CHECK-START: long Main.$noinline$longUnnecessaryShiftMasking(long, int) instruction_simplifier (after)
  /// CHECK:          <<Value:j\d+>>    ParameterValue
  /// CHECK:          <<Shift:i\d+>>    ParameterValue
  /// CHECK-DAG:      <<Shr:j\d+>>      Shr [<<Value>>,<<Shift>>]
  /// CHECK-DAG:                        Return [<<Shr>>]

  public static long $noinline$longUnnecessaryShiftMasking(long value, int shift) {
    return value >> (shift & 63);
  }

  /// CHECK-START: int Main.$noinline$intUnnecessaryWiderShiftMasking(int, int) instruction_simplifier (before)
  /// CHECK:          <<Value:i\d+>>    ParameterValue
  /// CHECK:          <<Shift:i\d+>>    ParameterValue
  /// CHECK-DAG:      <<Const255:i\d+>> IntConstant 255
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Shift>>,<<Const255>>]
  /// CHECK-DAG:      <<UShr:i\d+>>     UShr [<<Value>>,<<And>>]
  /// CHECK-DAG:                        Return [<<UShr>>]

  /// CHECK-START: int Main.$noinline$intUnnecessaryWiderShiftMasking(int, int) instruction_simplifier (after)
  /// CHECK:          <<Value:i\d+>>    ParameterValue
  /// CHECK:          <<Shift:i\d+>>    ParameterValue
  /// CHECK-DAG:      <<UShr:i\d+>>     UShr [<<Value>>,<<Shift>>]
  /// CHECK-DAG:                        Return [<<UShr>>]

  public static int $noinline$intUnnecessaryWiderShiftMasking(int value, int shift) {
    return value >>> (shift & 0xff);
  }

  /// CHECK-START: long Main.$noinline$longSmallerShiftMasking(long, int) instruction_simplifier (before)
  /// CHECK:          <<Value:j\d+>>    ParameterValue
  /// CHECK:          <<Shift:i\d+>>    ParameterValue
  /// CHECK-DAG:      <<Const3:i\d+>>   IntConstant 3
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Shift>>,<<Const3>>]
  /// CHECK-DAG:      <<Shl:j\d+>>      Shl [<<Value>>,<<And>>]
  /// CHECK-DAG:                        Return [<<Shl>>]

  /// CHECK-START: long Main.$noinline$longSmallerShiftMasking(long, int) instruction_simplifier (after)
  /// CHECK:          <<Value:j\d+>>    ParameterValue
  /// CHECK:          <<Shift:i\d+>>    ParameterValue
  /// CHECK-DAG:      <<Const3:i\d+>>   IntConstant 3
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Shift>>,<<Const3>>]
  /// CHECK-DAG:      <<Shl:j\d+>>      Shl [<<Value>>,<<And>>]
  /// CHECK-DAG:                        Return [<<Shl>>]

  public static long $noinline$longSmallerShiftMasking(long value, int shift) {
    return value << (shift & 3);
  }

  /// CHECK-START: int Main.$noinline$otherUseOfUnnecessaryShiftMasking(int, int) instruction_simplifier (before)
  /// CHECK:          <<Value:i\d+>>    ParameterValue
  /// CHECK:          <<Shift:i\d+>>    ParameterValue
  /// CHECK-DAG:      <<Const31:i\d+>>  IntConstant 31
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Shift>>,<<Const31>>]
  /// CHECK-DAG:      <<Shr:i\d+>>      Shr [<<Value>>,<<And>>]
  /// CHECK-DAG:      <<Add:i\d+>>      Add [<<Shr>>,<<And>>]
  /// CHECK-DAG:                        Return [<<Add>>]

  /// CHECK-START: int Main.$noinline$otherUseOfUnnecessaryShiftMasking(int, int) instruction_simplifier (after)
  /// CHECK:          <<Value:i\d+>>    ParameterValue
  /// CHECK:          <<Shift:i\d+>>    ParameterValue
  /// CHECK-DAG:      <<Const31:i\d+>>  IntConstant 31
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Shift>>,<<Const31>>]
  /// CHECK-DAG:      <<Shr:i\d+>>      Shr [<<Value>>,<<Shift>>]
  /// CHECK-DAG:      <<Add:i\d+>>      Add [<<Shr>>,<<And>>]
  /// CHECK-DAG:                        Return [<<Add>>]

  public static int $noinline$otherUseOfUnnecessaryShiftMasking(int value, int shift) {
    int temp = shift & 31;
    return (value >> temp) + temp;
  }

  /// CHECK-START: int Main.$noinline$intUnnecessaryShiftModifications(int, int) instruction_simplifier (before)
  /// CHECK:          <<Value:i\d+>>    ParameterValue
  /// CHECK:          <<Shift:i\d+>>    ParameterValue
  /// CHECK-DAG:      <<Const32:i\d+>>  IntConstant 32
  /// CHECK-DAG:      <<Const64:i\d+>>  IntConstant 64
  /// CHECK-DAG:      <<Const96:i\d+>>  IntConstant 96
  /// CHECK-DAG:      <<Const128:i\d+>> IntConstant 128
  /// CHECK-DAG:      <<Or:i\d+>>       Or [<<Shift>>,<<Const32>>]
  /// CHECK-DAG:      <<Xor:i\d+>>      Xor [<<Shift>>,<<Const64>>]
  /// CHECK-DAG:      <<Add:i\d+>>      Add [<<Shift>>,<<Const96>>]
  /// CHECK-DAG:      <<Sub:i\d+>>      Sub [<<Shift>>,<<Const128>>]
  /// CHECK-DAG:      <<Conv:b\d+>>     TypeConversion [<<Shift>>]
  /// CHECK-DAG:                        Shl [<<Value>>,<<Or>>]
  /// CHECK-DAG:                        Shr [<<Value>>,<<Xor>>]
  /// CHECK-DAG:                        UShr [<<Value>>,<<Add>>]
  /// CHECK-DAG:                        Shl [<<Value>>,<<Sub>>]
  /// CHECK-DAG:                        Shr [<<Value>>,<<Conv>>]

  /// CHECK-START: int Main.$noinline$intUnnecessaryShiftModifications(int, int) instruction_simplifier (after)
  /// CHECK:          <<Value:i\d+>>    ParameterValue
  /// CHECK:          <<Shift:i\d+>>    ParameterValue
  /// CHECK-DAG:                        Shl [<<Value>>,<<Shift>>]
  /// CHECK-DAG:                        Shr [<<Value>>,<<Shift>>]
  /// CHECK-DAG:                        UShr [<<Value>>,<<Shift>>]
  /// CHECK-DAG:                        Shl [<<Value>>,<<Shift>>]
  /// CHECK-DAG:                        Shr [<<Value>>,<<Shift>>]

  public static int $noinline$intUnnecessaryShiftModifications(int value, int shift) {
    int c128 = 128;
    return (value << (shift | 32)) +
           (value >> (shift ^ 64)) +
           (value >>> (shift + 96)) +
           (value << (shift - c128)) +  // Needs a named constant to generate Sub.
           (value >> ((byte) shift));
  }

  /// CHECK-START: int Main.$noinline$intNecessaryShiftModifications(int, int) instruction_simplifier (before)
  /// CHECK:          <<Value:i\d+>>    ParameterValue
  /// CHECK:          <<Shift:i\d+>>    ParameterValue
  /// CHECK-DAG:      <<Const33:i\d+>>  IntConstant 33
  /// CHECK-DAG:      <<Const65:i\d+>>  IntConstant 65
  /// CHECK-DAG:      <<Const97:i\d+>>  IntConstant 97
  /// CHECK-DAG:      <<Const129:i\d+>> IntConstant 129
  /// CHECK-DAG:      <<Or:i\d+>>       Or [<<Shift>>,<<Const33>>]
  /// CHECK-DAG:      <<Xor:i\d+>>      Xor [<<Shift>>,<<Const65>>]
  /// CHECK-DAG:      <<Add:i\d+>>      Add [<<Shift>>,<<Const97>>]
  /// CHECK-DAG:      <<Sub:i\d+>>      Sub [<<Shift>>,<<Const129>>]
  /// CHECK-DAG:                        Shl [<<Value>>,<<Or>>]
  /// CHECK-DAG:                        Shr [<<Value>>,<<Xor>>]
  /// CHECK-DAG:                        UShr [<<Value>>,<<Add>>]
  /// CHECK-DAG:                        Shl [<<Value>>,<<Sub>>]

  /// CHECK-START: int Main.$noinline$intNecessaryShiftModifications(int, int) instruction_simplifier (after)
  /// CHECK:          <<Value:i\d+>>    ParameterValue
  /// CHECK:          <<Shift:i\d+>>    ParameterValue
  /// CHECK-DAG:      <<Const33:i\d+>>  IntConstant 33
  /// CHECK-DAG:      <<Const65:i\d+>>  IntConstant 65
  /// CHECK-DAG:      <<Const97:i\d+>>  IntConstant 97
  /// CHECK-DAG:      <<Const129:i\d+>> IntConstant 129
  /// CHECK-DAG:      <<Or:i\d+>>       Or [<<Shift>>,<<Const33>>]
  /// CHECK-DAG:      <<Xor:i\d+>>      Xor [<<Shift>>,<<Const65>>]
  /// CHECK-DAG:      <<Add:i\d+>>      Add [<<Shift>>,<<Const97>>]
  /// CHECK-DAG:      <<Sub:i\d+>>      Sub [<<Shift>>,<<Const129>>]
  /// CHECK-DAG:                        Shl [<<Value>>,<<Or>>]
  /// CHECK-DAG:                        Shr [<<Value>>,<<Xor>>]
  /// CHECK-DAG:                        UShr [<<Value>>,<<Add>>]
  /// CHECK-DAG:                        Shl [<<Value>>,<<Sub>>]

  public static int $noinline$intNecessaryShiftModifications(int value, int shift) {
    int c129 = 129;
    return (value << (shift | 33)) +
           (value >> (shift ^ 65)) +
           (value >>> (shift + 97)) +
           (value << (shift - c129));  // Needs a named constant to generate Sub.
  }

  /// CHECK-START: int Main.$noinline$intAddSubSimplifyArg1(int, int) instruction_simplifier (before)
  /// CHECK:          <<X:i\d+>>        ParameterValue
  /// CHECK:          <<Y:i\d+>>        ParameterValue
  /// CHECK-DAG:      <<Sum:i\d+>>      Add [<<X>>,<<Y>>]
  /// CHECK-DAG:      <<Res:i\d+>>      Sub [<<Sum>>,<<X>>]
  /// CHECK-DAG:                        Return [<<Res>>]

  /// CHECK-START: int Main.$noinline$intAddSubSimplifyArg1(int, int) instruction_simplifier (after)
  /// CHECK:          <<X:i\d+>>        ParameterValue
  /// CHECK:          <<Y:i\d+>>        ParameterValue
  /// CHECK-DAG:      <<Sum:i\d+>>      Add [<<X>>,<<Y>>]
  /// CHECK-DAG:                        Return [<<Y>>]

  public static int $noinline$intAddSubSimplifyArg1(int x, int y) {
    int sum = x + y;
    return sum - x;
  }

  /// CHECK-START: int Main.$noinline$intAddSubSimplifyArg2(int, int) instruction_simplifier (before)
  /// CHECK:          <<X:i\d+>>        ParameterValue
  /// CHECK:          <<Y:i\d+>>        ParameterValue
  /// CHECK-DAG:      <<Sum:i\d+>>      Add [<<X>>,<<Y>>]
  /// CHECK-DAG:      <<Res:i\d+>>      Sub [<<Sum>>,<<Y>>]
  /// CHECK-DAG:                        Return [<<Res>>]

  /// CHECK-START: int Main.$noinline$intAddSubSimplifyArg2(int, int) instruction_simplifier (after)
  /// CHECK:          <<X:i\d+>>        ParameterValue
  /// CHECK:          <<Y:i\d+>>        ParameterValue
  /// CHECK-DAG:      <<Sum:i\d+>>      Add [<<X>>,<<Y>>]
  /// CHECK-DAG:                        Return [<<X>>]

  public static int $noinline$intAddSubSimplifyArg2(int x, int y) {
    int sum = x + y;
    return sum - y;
  }

  /// CHECK-START: int Main.$noinline$intSubAddSimplifyLeft(int, int) instruction_simplifier (before)
  /// CHECK:          <<X:i\d+>>        ParameterValue
  /// CHECK:          <<Y:i\d+>>        ParameterValue
  /// CHECK-DAG:      <<Sub:i\d+>>      Sub [<<X>>,<<Y>>]
  /// CHECK-DAG:      <<Res:i\d+>>      Add [<<Sub>>,<<Y>>]
  /// CHECK-DAG:                        Return [<<Res>>]

  /// CHECK-START: int Main.$noinline$intSubAddSimplifyLeft(int, int) instruction_simplifier (after)
  /// CHECK:          <<X:i\d+>>        ParameterValue
  /// CHECK:          <<Y:i\d+>>        ParameterValue
  /// CHECK-DAG:      <<Sub:i\d+>>      Sub [<<X>>,<<Y>>]
  /// CHECK-DAG:                        Return [<<X>>]

  public static int $noinline$intSubAddSimplifyLeft(int x, int y) {
    int sub = x - y;
    return sub + y;
  }

  /// CHECK-START: int Main.$noinline$intSubAddSimplifyRight(int, int) instruction_simplifier (before)
  /// CHECK:          <<X:i\d+>>        ParameterValue
  /// CHECK:          <<Y:i\d+>>        ParameterValue
  /// CHECK-DAG:      <<Sub:i\d+>>      Sub [<<X>>,<<Y>>]
  /// CHECK-DAG:      <<Res:i\d+>>      Add [<<Y>>,<<Sub>>]
  /// CHECK-DAG:                        Return [<<Res>>]

  /// CHECK-START: int Main.$noinline$intSubAddSimplifyRight(int, int) instruction_simplifier (after)
  /// CHECK:          <<X:i\d+>>        ParameterValue
  /// CHECK:          <<Y:i\d+>>        ParameterValue
  /// CHECK-DAG:      <<Sub:i\d+>>      Sub [<<X>>,<<Y>>]
  /// CHECK-DAG:                        Return [<<X>>]

  public static int $noinline$intSubAddSimplifyRight(int x, int y) {
    int sub = x - y;
    return y + sub;
  }

  /// CHECK-START: float Main.$noinline$floatAddSubSimplifyArg1(float, float) instruction_simplifier (before)
  /// CHECK:          <<X:f\d+>>        ParameterValue
  /// CHECK:          <<Y:f\d+>>        ParameterValue
  /// CHECK-DAG:      <<Sum:f\d+>>      Add [<<X>>,<<Y>>]
  /// CHECK-DAG:      <<Res:f\d+>>      Sub [<<Sum>>,<<X>>]
  /// CHECK-DAG:                        Return [<<Res>>]

  /// CHECK-START: float Main.$noinline$floatAddSubSimplifyArg1(float, float) instruction_simplifier (after)
  /// CHECK:          <<X:f\d+>>        ParameterValue
  /// CHECK:          <<Y:f\d+>>        ParameterValue
  /// CHECK-DAG:      <<Sum:f\d+>>      Add [<<X>>,<<Y>>]
  /// CHECK-DAG:      <<Res:f\d+>>      Sub [<<Sum>>,<<X>>]
  /// CHECK-DAG:                        Return [<<Res>>]

  public static float $noinline$floatAddSubSimplifyArg1(float x, float y) {
    float sum = x + y;
    return sum - x;
  }

  /// CHECK-START: float Main.$noinline$floatAddSubSimplifyArg2(float, float) instruction_simplifier (before)
  /// CHECK:          <<X:f\d+>>        ParameterValue
  /// CHECK:          <<Y:f\d+>>        ParameterValue
  /// CHECK-DAG:      <<Sum:f\d+>>      Add [<<X>>,<<Y>>]
  /// CHECK-DAG:      <<Res:f\d+>>      Sub [<<Sum>>,<<Y>>]
  /// CHECK-DAG:                        Return [<<Res>>]

  /// CHECK-START: float Main.$noinline$floatAddSubSimplifyArg2(float, float) instruction_simplifier (after)
  /// CHECK:          <<X:f\d+>>        ParameterValue
  /// CHECK:          <<Y:f\d+>>        ParameterValue
  /// CHECK-DAG:      <<Sum:f\d+>>      Add [<<X>>,<<Y>>]
  /// CHECK-DAG:      <<Res:f\d+>>      Sub [<<Sum>>,<<Y>>]
  /// CHECK-DAG:                        Return [<<Res>>]

  public static float $noinline$floatAddSubSimplifyArg2(float x, float y) {
    float sum = x + y;
    return sum - y;
  }

  /// CHECK-START: float Main.$noinline$floatSubAddSimplifyLeft(float, float) instruction_simplifier (before)
  /// CHECK:          <<X:f\d+>>        ParameterValue
  /// CHECK:          <<Y:f\d+>>        ParameterValue
  /// CHECK-DAG:      <<Sub:f\d+>>      Sub [<<X>>,<<Y>>]
  /// CHECK-DAG:      <<Res:f\d+>>      Add [<<Sub>>,<<Y>>]
  /// CHECK-DAG:                        Return [<<Res>>]

  /// CHECK-START: float Main.$noinline$floatSubAddSimplifyLeft(float, float) instruction_simplifier (after)
  /// CHECK:          <<X:f\d+>>        ParameterValue
  /// CHECK:          <<Y:f\d+>>        ParameterValue
  /// CHECK-DAG:      <<Sub:f\d+>>      Sub [<<X>>,<<Y>>]
  /// CHECK-DAG:      <<Res:f\d+>>      Add [<<Sub>>,<<Y>>]
  /// CHECK-DAG:                        Return [<<Res>>]

  public static float $noinline$floatSubAddSimplifyLeft(float x, float y) {
    float sub = x - y;
    return sub + y;
  }

  /// CHECK-START: float Main.$noinline$floatSubAddSimplifyRight(float, float) instruction_simplifier (before)
  /// CHECK:          <<X:f\d+>>        ParameterValue
  /// CHECK:          <<Y:f\d+>>        ParameterValue
  /// CHECK-DAG:      <<Sub:f\d+>>      Sub [<<X>>,<<Y>>]
  /// CHECK-DAG:      <<Res:f\d+>>      Add [<<Y>>,<<Sub>>]
  /// CHECK-DAG:                        Return [<<Res>>]

  /// CHECK-START: float Main.$noinline$floatSubAddSimplifyRight(float, float) instruction_simplifier (after)
  /// CHECK:          <<X:f\d+>>        ParameterValue
  /// CHECK:          <<Y:f\d+>>        ParameterValue
  /// CHECK-DAG:      <<Sub:f\d+>>      Sub [<<X>>,<<Y>>]
  /// CHECK-DAG:      <<Res:f\d+>>      Add [<<Y>>,<<Sub>>]
  /// CHECK-DAG:                        Return [<<Res>>]

  public static float $noinline$floatSubAddSimplifyRight(float x, float y) {
    float sub = x - y;
    return y + sub;
  }

  // Sub/Add and Sub/Sub simplifications

  /// CHECK-START: int Main.$noinline$testSubAddInt(int, int) instruction_simplifier (before)
  /// CHECK: <<x:i\d+>>   ParameterValue
  /// CHECK: <<y:i\d+>>   ParameterValue
  /// CHECK: <<add:i\d+>> Add [<<x>>,<<y>>]
  /// CHECK: <<sub:i\d+>> Sub [<<y>>,<<add>>]
  /// CHECK:              Return [<<sub>>]

  /// CHECK-START: int Main.$noinline$testSubAddInt(int, int) instruction_simplifier (after)
  /// CHECK: <<x:i\d+>>   ParameterValue
  /// CHECK: <<y:i\d+>>   ParameterValue
  /// CHECK: <<add:i\d+>> Add [<<x>>,<<y>>]
  /// CHECK: <<neg:i\d+>> Neg [<<x>>]
  /// CHECK:              Return [<<neg>>]

  /// CHECK-START: int Main.$noinline$testSubAddInt(int, int) instruction_simplifier (after)
  /// CHECK-NOT: Sub

  /// CHECK-START: int Main.$noinline$testSubAddInt(int, int) dead_code_elimination$initial (after)
  /// CHECK-NOT: Add
  static int $noinline$testSubAddInt(int x, int y) {
    return y - (x + y);
  }

  /// CHECK-START: int Main.$noinline$testSubAddOtherVersionInt(int, int) instruction_simplifier (before)
  /// CHECK: <<x:i\d+>>   ParameterValue
  /// CHECK: <<y:i\d+>>   ParameterValue
  /// CHECK: <<add:i\d+>> Add [<<x>>,<<y>>]
  /// CHECK: <<sub:i\d+>> Sub [<<x>>,<<add>>]
  /// CHECK:              Return [<<sub>>]

  /// CHECK-START: int Main.$noinline$testSubAddOtherVersionInt(int, int) instruction_simplifier (after)
  /// CHECK: <<x:i\d+>>   ParameterValue
  /// CHECK: <<y:i\d+>>   ParameterValue
  /// CHECK: <<add:i\d+>> Add [<<x>>,<<y>>]
  /// CHECK: <<neg:i\d+>> Neg [<<y>>]
  /// CHECK:              Return [<<neg>>]

  /// CHECK-START: int Main.$noinline$testSubAddOtherVersionInt(int, int) instruction_simplifier (after)
  /// CHECK-NOT: Sub

  /// CHECK-START: int Main.$noinline$testSubAddOtherVersionInt(int, int) dead_code_elimination$initial (after)
  /// CHECK-NOT: Add
  static int $noinline$testSubAddOtherVersionInt(int x, int y) {
    return x - (x + y);
  }

  /// CHECK-START: int Main.$noinline$testSubSubInt(int, int) instruction_simplifier (before)
  /// CHECK: <<x:i\d+>>    ParameterValue
  /// CHECK: <<y:i\d+>>    ParameterValue
  /// CHECK: <<sub1:i\d+>> Sub [<<x>>,<<y>>]
  /// CHECK: <<sub2:i\d+>> Sub [<<sub1>>,<<x>>]
  /// CHECK:               Return [<<sub2>>]

  /// CHECK-START: int Main.$noinline$testSubSubInt(int, int) instruction_simplifier (after)
  /// CHECK: <<x:i\d+>>    ParameterValue
  /// CHECK: <<y:i\d+>>    ParameterValue
  /// CHECK: <<sub1:i\d+>> Sub [<<x>>,<<y>>]
  /// CHECK: <<neg:i\d+>>  Neg [<<y>>]
  /// CHECK:               Return [<<neg>>]

  /// CHECK-START: int Main.$noinline$testSubSubInt(int, int) instruction_simplifier (after)
  /// CHECK:     Sub
  /// CHECK-NOT: Sub

  /// CHECK-START: int Main.$noinline$testSubSubInt(int, int) dead_code_elimination$initial (after)
  /// CHECK-NOT: Sub
  static int $noinline$testSubSubInt(int x, int y) {
    return (x - y) - x;
  }

  /// CHECK-START: int Main.$noinline$testSubSubOtherVersionInt(int, int) instruction_simplifier (before)
  /// CHECK: <<x:i\d+>>    ParameterValue
  /// CHECK: <<y:i\d+>>    ParameterValue
  /// CHECK: <<sub1:i\d+>> Sub [<<x>>,<<y>>]
  /// CHECK: <<sub2:i\d+>> Sub [<<x>>,<<sub1>>]
  /// CHECK:               Return [<<sub2>>]

  /// CHECK-START: int Main.$noinline$testSubSubOtherVersionInt(int, int) instruction_simplifier (after)
  /// CHECK: <<x:i\d+>>    ParameterValue
  /// CHECK: <<y:i\d+>>    ParameterValue
  /// CHECK: <<sub1:i\d+>> Sub [<<x>>,<<y>>]
  /// CHECK:               Return [<<y>>]

  /// CHECK-START: int Main.$noinline$testSubSubOtherVersionInt(int, int) instruction_simplifier (after)
  /// CHECK:     Sub
  /// CHECK-NOT: Sub

  /// CHECK-START: int Main.$noinline$testSubSubOtherVersionInt(int, int) dead_code_elimination$initial (after)
  /// CHECK-NOT: Sub
  static int $noinline$testSubSubOtherVersionInt(int x, int y) {
    return x - (x - y);
  }

  /// CHECK-START: long Main.$noinline$testSubAddLong(long, long) instruction_simplifier (before)
  /// CHECK: <<x:j\d+>>   ParameterValue
  /// CHECK: <<y:j\d+>>   ParameterValue
  /// CHECK: <<add:j\d+>> Add [<<x>>,<<y>>]
  /// CHECK: <<sub:j\d+>> Sub [<<y>>,<<add>>]
  /// CHECK:              Return [<<sub>>]

  /// CHECK-START: long Main.$noinline$testSubAddLong(long, long) instruction_simplifier (after)
  /// CHECK: <<x:j\d+>>   ParameterValue
  /// CHECK: <<y:j\d+>>   ParameterValue
  /// CHECK: <<add:j\d+>> Add [<<x>>,<<y>>]
  /// CHECK: <<neg:j\d+>> Neg [<<x>>]
  /// CHECK:              Return [<<neg>>]

  /// CHECK-START: long Main.$noinline$testSubAddLong(long, long) instruction_simplifier (after)
  /// CHECK-NOT: Sub

  /// CHECK-START: long Main.$noinline$testSubAddLong(long, long) dead_code_elimination$initial (after)
  /// CHECK-NOT: Add
  static long $noinline$testSubAddLong(long x, long y) {
    return y - (x + y);
  }

  /// CHECK-START: long Main.$noinline$testSubAddOtherVersionLong(long, long) instruction_simplifier (before)
  /// CHECK: <<x:j\d+>>   ParameterValue
  /// CHECK: <<y:j\d+>>   ParameterValue
  /// CHECK: <<add:j\d+>> Add [<<x>>,<<y>>]
  /// CHECK: <<sub:j\d+>> Sub [<<x>>,<<add>>]
  /// CHECK:              Return [<<sub>>]

  /// CHECK-START: long Main.$noinline$testSubAddOtherVersionLong(long, long) instruction_simplifier (after)
  /// CHECK: <<x:j\d+>>   ParameterValue
  /// CHECK: <<y:j\d+>>   ParameterValue
  /// CHECK: <<add:j\d+>> Add [<<x>>,<<y>>]
  /// CHECK: <<neg:j\d+>> Neg [<<y>>]
  /// CHECK:              Return [<<neg>>]

  /// CHECK-START: long Main.$noinline$testSubAddOtherVersionLong(long, long) instruction_simplifier (after)
  /// CHECK-NOT: Sub

  /// CHECK-START: long Main.$noinline$testSubAddOtherVersionLong(long, long) dead_code_elimination$initial (after)
  /// CHECK-NOT: Add
  static long $noinline$testSubAddOtherVersionLong(long x, long y) {
    return x - (x + y);
  }

  /// CHECK-START: long Main.$noinline$testSubSubLong(long, long) instruction_simplifier (before)
  /// CHECK: <<x:j\d+>>    ParameterValue
  /// CHECK: <<y:j\d+>>    ParameterValue
  /// CHECK: <<sub1:j\d+>> Sub [<<x>>,<<y>>]
  /// CHECK: <<sub2:j\d+>> Sub [<<sub1>>,<<x>>]
  /// CHECK:               Return [<<sub2>>]

  /// CHECK-START: long Main.$noinline$testSubSubLong(long, long) instruction_simplifier (after)
  /// CHECK: <<x:j\d+>>    ParameterValue
  /// CHECK: <<y:j\d+>>    ParameterValue
  /// CHECK: <<sub1:j\d+>> Sub [<<x>>,<<y>>]
  /// CHECK: <<neg:j\d+>>  Neg [<<y>>]
  /// CHECK:               Return [<<neg>>]

  /// CHECK-START: long Main.$noinline$testSubSubLong(long, long) instruction_simplifier (after)
  /// CHECK:     Sub
  /// CHECK-NOT: Sub

  /// CHECK-START: long Main.$noinline$testSubSubLong(long, long) dead_code_elimination$initial (after)
  /// CHECK-NOT: Sub
  static long $noinline$testSubSubLong(long x, long y) {
    return (x - y) - x;
  }

  /// CHECK-START: long Main.$noinline$testSubSubOtherVersionLong(long, long) instruction_simplifier (before)
  /// CHECK: <<x:j\d+>>    ParameterValue
  /// CHECK: <<y:j\d+>>    ParameterValue
  /// CHECK: <<sub1:j\d+>> Sub [<<x>>,<<y>>]
  /// CHECK: <<sub2:j\d+>> Sub [<<x>>,<<sub1>>]
  /// CHECK:               Return [<<sub2>>]

  /// CHECK-START: long Main.$noinline$testSubSubOtherVersionLong(long, long) instruction_simplifier (after)
  /// CHECK: <<x:j\d+>>    ParameterValue
  /// CHECK: <<y:j\d+>>    ParameterValue
  /// CHECK: <<sub1:j\d+>> Sub [<<x>>,<<y>>]
  /// CHECK:               Return [<<y>>]

  /// CHECK-START: long Main.$noinline$testSubSubOtherVersionLong(long, long) instruction_simplifier (after)
  /// CHECK:     Sub
  /// CHECK-NOT: Sub

  /// CHECK-START: long Main.$noinline$testSubSubOtherVersionLong(long, long) dead_code_elimination$initial (after)
  /// CHECK-NOT: Sub
  static long $noinline$testSubSubOtherVersionLong(long x, long y) {
    return x - (x - y);
  }

  /// CHECK-START: int Main.$noinline$getUint8FromInstanceByteField(Main) instruction_simplifier (before)
  /// CHECK-DAG:      <<Const255:i\d+>> IntConstant 255
  /// CHECK-DAG:      <<Get:b\d+>>      InstanceFieldGet
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Get>>,<<Const255>>]
  /// CHECK-DAG:                        Return [<<And>>]

  /// CHECK-START: int Main.$noinline$getUint8FromInstanceByteField(Main) instruction_simplifier (after)
  /// CHECK-DAG:      <<Get:a\d+>>      InstanceFieldGet
  /// CHECK-DAG:                        Return [<<Get>>]

  /// CHECK-START: int Main.$noinline$getUint8FromInstanceByteField(Main) instruction_simplifier (after)
  /// CHECK-NOT:                        And
  /// CHECK-NOT:                        TypeConversion
  public static int $noinline$getUint8FromInstanceByteField(Main m) {
    return m.instanceByteField & 0xff;
  }

  /// CHECK-START: int Main.$noinline$getUint8FromStaticByteField() instruction_simplifier (before)
  /// CHECK-DAG:      <<Const255:i\d+>> IntConstant 255
  /// CHECK-DAG:      <<Get:b\d+>>      StaticFieldGet
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Get>>,<<Const255>>]
  /// CHECK-DAG:                        Return [<<And>>]

  /// CHECK-START: int Main.$noinline$getUint8FromStaticByteField() instruction_simplifier (after)
  /// CHECK-DAG:      <<Get:a\d+>>      StaticFieldGet
  /// CHECK-DAG:                        Return [<<Get>>]

  /// CHECK-START: int Main.$noinline$getUint8FromStaticByteField() instruction_simplifier (after)
  /// CHECK-NOT:                        And
  /// CHECK-NOT:                        TypeConversion
  public static int $noinline$getUint8FromStaticByteField() {
    return staticByteField & 0xff;
  }

  /// CHECK-START: int Main.$noinline$getUint8FromByteArray(byte[]) instruction_simplifier (before)
  /// CHECK-DAG:      <<Const255:i\d+>> IntConstant 255
  /// CHECK-DAG:      <<Get:b\d+>>      ArrayGet
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Get>>,<<Const255>>]
  /// CHECK-DAG:                        Return [<<And>>]

  /// CHECK-START: int Main.$noinline$getUint8FromByteArray(byte[]) instruction_simplifier (after)
  /// CHECK-DAG:      <<Get:a\d+>>      ArrayGet
  /// CHECK-DAG:                        Return [<<Get>>]

  /// CHECK-START: int Main.$noinline$getUint8FromByteArray(byte[]) instruction_simplifier (after)
  /// CHECK-NOT:                        And
  /// CHECK-NOT:                        TypeConversion
  public static int $noinline$getUint8FromByteArray(byte[] a) {
    return a[0] & 0xff;
  }

  /// CHECK-START: int Main.$noinline$getUint16FromInstanceShortField(Main) instruction_simplifier (before)
  /// CHECK-DAG:      <<Cst65535:i\d+>> IntConstant 65535
  /// CHECK-DAG:      <<Get:s\d+>>      InstanceFieldGet
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Get>>,<<Cst65535>>]
  /// CHECK-DAG:                        Return [<<And>>]

  /// CHECK-START: int Main.$noinline$getUint16FromInstanceShortField(Main) instruction_simplifier (after)
  /// CHECK-DAG:      <<Get:c\d+>>      InstanceFieldGet
  /// CHECK-DAG:                        Return [<<Get>>]

  /// CHECK-START: int Main.$noinline$getUint16FromInstanceShortField(Main) instruction_simplifier (after)
  /// CHECK-NOT:                        And
  /// CHECK-NOT:                        TypeConversion
  public static int $noinline$getUint16FromInstanceShortField(Main m) {
    return m.instanceShortField & 0xffff;
  }

  /// CHECK-START: int Main.$noinline$getUint16FromStaticShortField() instruction_simplifier (before)
  /// CHECK-DAG:      <<Cst65535:i\d+>> IntConstant 65535
  /// CHECK-DAG:      <<Get:s\d+>>      StaticFieldGet
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Get>>,<<Cst65535>>]
  /// CHECK-DAG:                        Return [<<And>>]

  /// CHECK-START: int Main.$noinline$getUint16FromStaticShortField() instruction_simplifier (after)
  /// CHECK-DAG:      <<Get:c\d+>>      StaticFieldGet
  /// CHECK-DAG:                        Return [<<Get>>]

  /// CHECK-START: int Main.$noinline$getUint16FromStaticShortField() instruction_simplifier (after)
  /// CHECK-NOT:                        And
  /// CHECK-NOT:                        TypeConversion
  public static int $noinline$getUint16FromStaticShortField() {
    return staticShortField & 0xffff;
  }

  /// CHECK-START: int Main.$noinline$getUint16FromShortArray(short[]) instruction_simplifier (before)
  /// CHECK-DAG:      <<Cst65535:i\d+>> IntConstant 65535
  /// CHECK-DAG:      <<Get:s\d+>>      ArrayGet
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Get>>,<<Cst65535>>]
  /// CHECK-DAG:                        Return [<<And>>]

  /// CHECK-START: int Main.$noinline$getUint16FromShortArray(short[]) instruction_simplifier (after)
  /// CHECK-DAG:      <<Get:c\d+>>      ArrayGet
  /// CHECK-DAG:                        Return [<<Get>>]

  /// CHECK-START: int Main.$noinline$getUint16FromShortArray(short[]) instruction_simplifier (after)
  /// CHECK-NOT:                        And
  /// CHECK-NOT:                        TypeConversion
  public static int $noinline$getUint16FromShortArray(short[] a) {
    return a[0] & 0xffff;
  }

  /// CHECK-START: int Main.$noinline$getInt16FromInstanceCharField(Main) instruction_simplifier (before)
  /// CHECK-DAG:      <<Get:c\d+>>      InstanceFieldGet
  /// CHECK-DAG:      <<Conv:s\d+>>     TypeConversion [<<Get>>]
  /// CHECK-DAG:                        Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$getInt16FromInstanceCharField(Main) instruction_simplifier (after)
  /// CHECK-DAG:      <<Get:s\d+>>      InstanceFieldGet
  /// CHECK-DAG:                        Return [<<Get>>]

  /// CHECK-START: int Main.$noinline$getInt16FromInstanceCharField(Main) instruction_simplifier (after)
  /// CHECK-NOT:                        And
  /// CHECK-NOT:                        TypeConversion
  public static int $noinline$getInt16FromInstanceCharField(Main m) {
    return (short) m.instanceCharField;
  }

  /// CHECK-START: int Main.$noinline$getInt16FromStaticCharField() instruction_simplifier (before)
  /// CHECK-DAG:      <<Get:c\d+>>      StaticFieldGet
  /// CHECK-DAG:      <<Conv:s\d+>>     TypeConversion [<<Get>>]
  /// CHECK-DAG:                        Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$getInt16FromStaticCharField() instruction_simplifier (after)
  /// CHECK-DAG:      <<Get:s\d+>>      StaticFieldGet
  /// CHECK-DAG:                        Return [<<Get>>]

  /// CHECK-START: int Main.$noinline$getInt16FromStaticCharField() instruction_simplifier (after)
  /// CHECK-NOT:                        And
  /// CHECK-NOT:                        TypeConversion
  public static int $noinline$getInt16FromStaticCharField() {
    return (short) staticCharField;
  }

  /// CHECK-START: int Main.$noinline$getInt16FromCharArray(char[]) instruction_simplifier (before)
  /// CHECK-DAG:      <<Get:c\d+>>      ArrayGet
  /// CHECK-DAG:      <<Conv:s\d+>>     TypeConversion [<<Get>>]
  /// CHECK-DAG:                        Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$getInt16FromCharArray(char[]) instruction_simplifier (after)
  /// CHECK-DAG:      <<Get:s\d+>>      ArrayGet
  /// CHECK-DAG:                        Return [<<Get>>]

  /// CHECK-START: int Main.$noinline$getInt16FromCharArray(char[]) instruction_simplifier (after)
  /// CHECK-NOT:                        And
  /// CHECK-NOT:                        TypeConversion
  public static int $noinline$getInt16FromCharArray(char[] a) {
    return (short) a[0];
  }

  /// CHECK-START: int Main.$noinline$byteToUint8AndBack() instruction_simplifier (before)
  /// CHECK-DAG:      <<Const255:i\d+>> IntConstant 255
  /// CHECK-DAG:      <<Get:b\d+>>      StaticFieldGet
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Get>>,<<Const255>>]
  /// CHECK-DAG:      <<Invoke:i\d+>>   InvokeStaticOrDirect [<<And>>{{(,[ij]\d+)?}}]
  /// CHECK-DAG:                        Return [<<Invoke>>]

  /// CHECK-START: int Main.$noinline$byteToUint8AndBack() instruction_simplifier (after)
  /// CHECK-DAG:      <<Get:a\d+>>      StaticFieldGet
  /// CHECK-DAG:      <<Invoke:i\d+>>   InvokeStaticOrDirect [<<Get>>{{(,[ij]\d+)?}}]
  /// CHECK-DAG:                        Return [<<Invoke>>]

  /// CHECK-START: int Main.$noinline$byteToUint8AndBack() instruction_simplifier$after_inlining (before)
  /// CHECK-DAG:      <<Get:a\d+>>      StaticFieldGet
  /// CHECK-DAG:      <<Conv:b\d+>>     TypeConversion [<<Get>>]
  /// CHECK-DAG:                        Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$byteToUint8AndBack() instruction_simplifier$after_inlining (after)
  /// CHECK-DAG:      <<Get:b\d+>>      StaticFieldGet
  /// CHECK-DAG:                        Return [<<Get>>]
  public static int $noinline$byteToUint8AndBack() {
    return $inline$toByte(staticByteField & 0xff);
  }

  public static int $inline$toByte(int value) {
    return (byte) value;
  }

  /// CHECK-START: int Main.$noinline$getStaticCharFieldAnd0xff() instruction_simplifier (before)
  /// CHECK-DAG:      <<Const255:i\d+>> IntConstant 255
  /// CHECK-DAG:      <<Get:c\d+>>      StaticFieldGet
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Get>>,<<Const255>>]
  /// CHECK-DAG:                        Return [<<And>>]

  /// CHECK-START: int Main.$noinline$getStaticCharFieldAnd0xff() instruction_simplifier (after)
  /// CHECK-DAG:      <<Const255:i\d+>> IntConstant 255
  /// CHECK-DAG:      <<Get:c\d+>>      StaticFieldGet
  /// CHECK-DAG:      <<Cnv:a\d+>>      TypeConversion [<<Get>>]
  /// CHECK-DAG:                        Return [<<Cnv>>]

  /// CHECK-START: int Main.$noinline$getStaticCharFieldAnd0xff() instruction_simplifier (after)
  /// CHECK-NOT:      {{a\d+}}          StaticFieldGet
  public static int $noinline$getStaticCharFieldAnd0xff() {
    return staticCharField & 0xff;
  }

  /// CHECK-START: int Main.$noinline$getUint8FromInstanceByteFieldWithAnotherUse(Main) instruction_simplifier (before)
  /// CHECK-DAG:      <<Const8:i\d+>>   IntConstant 8
  /// CHECK-DAG:      <<Const255:i\d+>> IntConstant 255
  /// CHECK-DAG:      <<Get:b\d+>>      InstanceFieldGet
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Get>>,<<Const255>>]
  /// CHECK-DAG:      <<Shl:i\d+>>      Shl [<<Get>>,<<Const8>>]
  /// CHECK-DAG:      <<Add:i\d+>>      Add [<<And>>,<<Shl>>]
  /// CHECK-DAG:                        Return [<<Add>>]

  /// CHECK-START: int Main.$noinline$getUint8FromInstanceByteFieldWithAnotherUse(Main) instruction_simplifier (after)
  /// CHECK-DAG:      <<Const8:i\d+>>   IntConstant 8
  /// CHECK-DAG:      <<Const255:i\d+>> IntConstant 255
  /// CHECK-DAG:      <<Get:b\d+>>      InstanceFieldGet
  /// CHECK-DAG:      <<Cnv:a\d+>>      TypeConversion [<<Get>>]
  /// CHECK-DAG:      <<Shl:i\d+>>      Shl [<<Get>>,<<Const8>>]
  /// CHECK-DAG:      <<Add:i\d+>>      Add [<<Cnv>>,<<Shl>>]
  /// CHECK-DAG:                        Return [<<Add>>]

  /// CHECK-START: int Main.$noinline$getUint8FromInstanceByteFieldWithAnotherUse(Main) instruction_simplifier (after)
  /// CHECK-NOT:      {{a\d+}}          InstanceFieldGet
  public static int $noinline$getUint8FromInstanceByteFieldWithAnotherUse(Main m) {
    byte b = m.instanceByteField;
    int v1 = b & 0xff;
    int v2 = (b << 8);
    return v1 + v2;
  }

  /// CHECK-START: int Main.$noinline$intAnd0xffToChar(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const255:i\d+>> IntConstant 255
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Arg>>,<<Const255>>]
  /// CHECK-DAG:      <<Conv:c\d+>>     TypeConversion [<<And>>]
  /// CHECK-DAG:                        Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$intAnd0xffToChar(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Conv:a\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:                        Return [<<Conv>>]
  public static int $noinline$intAnd0xffToChar(int value) {
    return (char) (value & 0xff);
  }

  /// CHECK-START: int Main.$noinline$intAnd0x1ffToChar(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const511:i\d+>> IntConstant 511
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Arg>>,<<Const511>>]
  /// CHECK-DAG:      <<Conv:c\d+>>     TypeConversion [<<And>>]
  /// CHECK-DAG:                        Return [<<Conv>>]

  // TODO: Simplify this. Unlike the $noinline$intAnd0xffToChar(), the TypeConversion
  // to `char` is not eliminated despite the result of the And being within the `char` range.

  // CHECK-START: int Main.$noinline$intAnd0x1ffToChar(int) instruction_simplifier (after)
  // CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  // CHECK-DAG:      <<Const511:i\d+>> IntConstant 511
  // CHECK-DAG:      <<And:i\d+>>      And [<<Arg>>,<<Const511>>]
  // CHECK-DAG:                        Return [<<And>>]
  public static int $noinline$intAnd0x1ffToChar(int value) {
    return (char) (value & 0x1ff);
  }

  /// CHECK-START: int Main.$noinline$getInstanceCharFieldAnd0x1ffff(Main) instruction_simplifier (before)
  /// CHECK-DAG:      <<Cst1ffff:i\d+>> IntConstant 131071
  /// CHECK-DAG:      <<Get:c\d+>>      InstanceFieldGet
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Get>>,<<Cst1ffff>>]
  /// CHECK-DAG:                        Return [<<And>>]

  /// CHECK-START: int Main.$noinline$getInstanceCharFieldAnd0x1ffff(Main) instruction_simplifier (after)
  /// CHECK-DAG:      <<Get:c\d+>>      InstanceFieldGet
  /// CHECK-DAG:                        Return [<<Get>>]
  public static int $noinline$getInstanceCharFieldAnd0x1ffff(Main m) {
    return m.instanceCharField & 0x1ffff;
  }

  /// CHECK-START: int Main.$noinline$bug68142795Byte(byte) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:b\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const:i\d+>>    IntConstant 255
  /// CHECK-DAG:      <<And1:i\d+>>     And [<<Arg>>,<<Const>>]
  /// CHECK-DAG:      <<And2:i\d+>>     And [<<And1>>,<<Const>>]
  /// CHECK-DAG:      <<Conv:b\d+>>     TypeConversion [<<And2>>]
  /// CHECK-DAG:                        Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$bug68142795Byte(byte) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:b\d+>>      ParameterValue
  /// CHECK-DAG:                        Return [<<Arg>>]
  public static int $noinline$bug68142795Byte(byte b) {
    return (byte)(0xff & (b & 0xff));
  }

  /// CHECK-START: int Main.$noinline$bug68142795Elaborate(byte) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:b\d+>>      ParameterValue
  /// CHECK-DAG:      <<Int255:i\d+>>   IntConstant 255
  /// CHECK-DAG:      <<Long255:j\d+>>  LongConstant 255
  /// CHECK-DAG:      <<And1:i\d+>>     And [<<Arg>>,<<Int255>>]
  /// CHECK-DAG:      <<Conv1:j\d+>>    TypeConversion [<<And1>>]
  /// CHECK-DAG:      <<And2:j\d+>>     And [<<Conv1>>,<<Long255>>]
  /// CHECK-DAG:      <<Conv2:i\d+>>    TypeConversion [<<And2>>]
  /// CHECK-DAG:      <<Conv3:b\d+>>    TypeConversion [<<Conv2>>]
  /// CHECK-DAG:                        Return [<<Conv3>>]

  /// CHECK-START: int Main.$noinline$bug68142795Elaborate(byte) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:b\d+>>      ParameterValue
  /// CHECK-DAG:                        Return [<<Arg>>]
  public static int $noinline$bug68142795Elaborate(byte b) {
    return (byte)((int)(((long)(b & 0xff)) & 255L));
  }

  /// CHECK-START: int Main.$noinline$emptyStringIndexOf(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Empty:l\d+>>    LoadString
  /// CHECK-DAG:      <<Equals:i\d+>>   InvokeVirtual [<<Empty>>,<<Arg>>] intrinsic:StringIndexOf
  /// CHECK-DAG:                        Return [<<Equals>>]

  /// CHECK-START: int Main.$noinline$emptyStringIndexOf(int) instruction_simplifier (after)
  /// CHECK-NOT:                        InvokeVirtual

  /// CHECK-START: int Main.$noinline$emptyStringIndexOf(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Minus1:i\d+>>   IntConstant -1
  /// CHECK-DAG:                        Return [<<Minus1>>]
  public static int $noinline$emptyStringIndexOf(int ch) {
    return "".indexOf(ch);
  }

  /// CHECK-START: int Main.$noinline$emptyStringIndexOfAfter(int, int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:      <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:      <<Empty:l\d+>>    LoadString
  /// CHECK-DAG:      <<Equals:i\d+>>   InvokeVirtual [<<Empty>>,<<Arg1>>,<<Arg2>>] intrinsic:StringIndexOfAfter
  /// CHECK-DAG:                        Return [<<Equals>>]

  /// CHECK-START: int Main.$noinline$emptyStringIndexOfAfter(int, int) instruction_simplifier (after)
  /// CHECK-NOT:                        InvokeVirtual

  /// CHECK-START: int Main.$noinline$emptyStringIndexOfAfter(int, int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Minus1:i\d+>>   IntConstant -1
  /// CHECK-DAG:                        Return [<<Minus1>>]
  public static int $noinline$emptyStringIndexOfAfter(int ch, int fromIndex) {
    return "".indexOf(ch, fromIndex);
  }

  /// CHECK-START: int Main.$noinline$singleCharStringIndexOf(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Empty:l\d+>>    LoadString
  /// CHECK-DAG:      <<Equals:i\d+>>   InvokeVirtual [<<Empty>>,<<Arg>>] intrinsic:StringIndexOf
  /// CHECK-DAG:                        Return [<<Equals>>]

  /// CHECK-START: int Main.$noinline$singleCharStringIndexOf(int) instruction_simplifier (after)
  /// CHECK-NOT:                        InvokeVirtual

  /// CHECK-START: int Main.$noinline$singleCharStringIndexOf(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<x:i\d+>>        IntConstant 120
  /// CHECK-DAG:      <<Zero:i\d+>>     IntConstant 0
  /// CHECK-DAG:      <<Minus1:i\d+>>   IntConstant -1
  /// CHECK-DAG:      <<Eq:z\d+>>       Equal [<<Arg>>,<<x>>]
  /// CHECK-DAG:      <<Select:i\d+>>   Select [<<Minus1>>,<<Zero>>,<<Eq>>]
  /// CHECK-DAG:                        Return [<<Select>>]
  public static int $noinline$singleCharStringIndexOf(int ch) {
    return "x".indexOf(ch);
  }

  /// CHECK-START: int Main.$noinline$singleCharStringIndexOfAfter(int, int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:      <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:      <<Empty:l\d+>>    LoadString
  /// CHECK-DAG:      <<Equals:i\d+>>   InvokeVirtual [<<Empty>>,<<Arg1>>,<<Arg2>>] intrinsic:StringIndexOfAfter
  /// CHECK-DAG:                        Return [<<Equals>>]

  /// CHECK-START: int Main.$noinline$singleCharStringIndexOfAfter(int, int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:      <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:      <<Empty:l\d+>>    LoadString
  /// CHECK-DAG:      <<Equals:i\d+>>   InvokeVirtual [<<Empty>>,<<Arg1>>,<<Arg2>>] intrinsic:StringIndexOfAfter
  /// CHECK-DAG:                        Return [<<Equals>>]
  public static int $noinline$singleCharStringIndexOfAfter(int ch, int fromIndex) {
    return "x".indexOf(ch, fromIndex);  // Not simplified.
  }

  /// CHECK-START: byte Main.$noinline$redundantAndNotRedundant(int) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndNotRedundant(int) instruction_simplifier (after)
  /// CHECK-DAG:                       And
  public static byte $noinline$redundantAndNotRedundant(int value) {
    // Here the AND is not redundant. This test checks that it is not removed.
    // 0xf500 = 1111 0101 0000 0000 - bits [11:8] of value will be changed by
    // the AND with 0101. These bits are kept following the SHR and TypeConversion.
    return (byte) ((value & 0xf500) >> 8);
  }

  /// CHECK-START:  byte Main.$noinline$redundantAndOtherUse(int) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START:  byte Main.$noinline$redundantAndOtherUse(int) instruction_simplifier (after)
  /// CHECK-DAG:                       And
  public static byte $noinline$redundantAndOtherUse(int value){
    int v1 = value & 0xff00;
    // Above AND is redundant in the context of calculating v2.
    byte v2 = (byte) (v1 >> 8);
    byte v3 = (byte) (v1 - 0x6eef);
    // Use of AND result (v1) in calculating v3 means AND must not be removed.
    return (byte) (v2 - v3);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShift2(short) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShift2(short) instruction_simplifier (after)
  /// CHECK-NOT:                       And
  public static byte $noinline$redundantAndShortToByteShift2(short value) {
    // & 0xfff only changes bits higher than bit number 12 inclusive.
    // These bits are discarded during Type Conversion to byte.
    // Therefore AND is redundant and should be removed.
    return (byte) ((value & 0xfff) >> 2);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShift5(short) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShift5(short) instruction_simplifier (after)
  /// CHECK-NOT:                       And
  public static byte $noinline$redundantAndShortToByteShift5(short value) {
    // & 0xffe0 changes bits [4:0] and bits higher than 16 inclusive.
    // These bits are discarded by the right shift and type conversion.
    // Therefore AND is redundant and should be removed.
    return (byte) ((value & 0xffe0) >> 5);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShift8(short) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShift8(short) instruction_simplifier (after)
  /// CHECK-NOT:                       And
  public static byte $noinline$redundantAndShortToByteShift8(short value) {
    return (byte) ((value & 0xff00) >> 8);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShift9NotRedundant(short) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShift9NotRedundant(short) instruction_simplifier (after)
  /// CHECK-DAG:                       And
  public static byte $noinline$redundantAndShortToByteShift9NotRedundant(short value) {
    // Byte and Short operands for bitwise operations (e.g. &,>>) are promoted to Int32 prior to operation.
    // Negative operands will be sign extended accordingly: (short: 0xff45) --> (int: 0xffffff45)
    // & fe00 will clear bits [31:16]. For negative 'value', this will clear sign bits.
    // Without the AND, the right shift will move the sign extended ones into the result instead of zeros.
    // Therefore AND is not redundant and should not be removed.
    return (byte) ((value & 0xfe00) >> 9);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShift10NotRedundant(short) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShift10NotRedundant(short) instruction_simplifier (after)
  /// CHECK-DAG:                       And
  public static byte $noinline$redundantAndShortToByteShift10NotRedundant(short value) {
    return (byte) ((value & 0x1ffff) >> 10);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShift12(short) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShift12(short) instruction_simplifier (after)
  /// CHECK-NOT:                       And
  public static byte $noinline$redundantAndShortToByteShift12(short value) {
    // & fff00 preserves bits [19:8] and clears all others.
    // Here the AND preserves enough ones; such that zeros are moved into the result.
    // Therefore AND is redundant and can be removed.
    return (byte) ((value & 0xfff00) >> 12);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShift15(short) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShift15(short) instruction_simplifier (after)
  /// CHECK-NOT:                       And
  public static byte $noinline$redundantAndShortToByteShift15(short value) {
    return (byte) ((value & 0xffff00) >> 15);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShift16(short) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShift16(short) instruction_simplifier (after)
  /// CHECK-NOT:                       And
  public static byte $noinline$redundantAndShortToByteShift16(short value) {
    return (byte) ((value & 0xf0ffff00) >> 16);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift2(int) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift2(int) instruction_simplifier (after)
  /// CHECK-NOT:                       And
  public static byte $noinline$redundantAndIntToByteShift2(int value) {
    return (byte) ((value & 0xfff) >> 2);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift5(int) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift5(int) instruction_simplifier (after)
  /// CHECK-NOT:                       And
  public static byte $noinline$redundantAndIntToByteShift5(int value) {
    return (byte) ((value & 0xffe0) >> 5);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift8(int) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift8(int) instruction_simplifier (after)
  /// CHECK-NOT:                       And
  public static byte $noinline$redundantAndIntToByteShift8(int value) {
    return (byte) ((value & 0xffffff00) >> 8);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift9(int) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift9(int) instruction_simplifier (after)
  /// CHECK-NOT:                       And
  public static byte $noinline$redundantAndIntToByteShift9(int value) {
    return (byte) ((value & 0xf001fe00) >> 9);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift16(int) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift16(int) instruction_simplifier (after)
  /// CHECK-NOT:                       And
  public static byte $noinline$redundantAndIntToByteShift16(int value) {
    return (byte) ((value & 0xf0ff0000) >> 16);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift20(int) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift20(int) instruction_simplifier (after)
  /// CHECK-NOT:                       And
  public static byte $noinline$redundantAndIntToByteShift20(int value) {
    return (byte) ((value & 0xfff00000) >> 20);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift25(int) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift25(int) instruction_simplifier (after)
  /// CHECK-NOT:                       And
  public static byte $noinline$redundantAndIntToByteShift25(int value) {
    return (byte) ((value & 0xfe000000) >> 25);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift24(int) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift24(int) instruction_simplifier (after)
  /// CHECK-NOT:                       And
  public static byte $noinline$redundantAndIntToByteShift24(int value) {
    return (byte) ((value & 0xff000000) >> 24);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift31(int) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShift31(int) instruction_simplifier (after)
  /// CHECK-NOT:                       And
  public static byte $noinline$redundantAndIntToByteShift31(int value) {
    return (byte) ((value & 0xff000000) >> 31);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShortAndConstant(short) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndShortToByteShortAndConstant(short) instruction_simplifier (after)
  /// CHECK-NOT:                       And
  public static byte $noinline$redundantAndShortToByteShortAndConstant(short value) {
    short and_constant = (short) 0xff00;
    return (byte) ((value & and_constant) >> 16);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShortAndConstant(int) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndIntToByteShortAndConstant(int) instruction_simplifier (after)
  /// CHECK-NOT:                       And
  public static byte $noinline$redundantAndIntToByteShortAndConstant(int value) {
    short and_constant = (short) 0xff00;
    return (byte) ((value & and_constant) >> 16);
  }

  /// CHECK-START: byte Main.$noinline$redundantAndRegressionNotConstant(int, int) instruction_simplifier (before)
  /// CHECK-DAG:                       And

  /// CHECK-START: byte Main.$noinline$redundantAndRegressionNotConstant(int, int) instruction_simplifier (after)
  /// CHECK-DAG:                       And
  public static byte $noinline$redundantAndRegressionNotConstant(int value, int mask) {
    return (byte) ((value & mask) >> 8);
  }

  /// CHECK-START: int Main.$noinline$deadAddAfterUnrollingAndSimplification(int[]) dead_code_elimination$before_codegen (before)
  /// CHECK-DAG: <<Param:l\d+>>     ParameterValue                             loop:none
  /// CHECK-DAG: <<Const0:i\d+>>    IntConstant 0                              loop:none
  /// CHECK-DAG: <<Const1:i\d+>>    IntConstant 1                              loop:none
  /// CHECK-DAG: <<Const2:i\d+>>    IntConstant 2                              loop:none
  /// CHECK-DAG: <<IndexPhi:i\d+>>  Phi [<<Const0>>,{{i\d+}}]                  loop:<<Loop:B\d+>> outer_loop:none
  //            Induction variable:
  /// CHECK-DAG:                    Add [<<IndexPhi>>,<<Const2>>]              loop:<<Loop>>      outer_loop:none
  //            Array Element Addition:
  /// CHECK-DAG: <<Store1:i\d+>>    Add [{{i\d+}},<<Const1>>]                  loop:<<Loop>>      outer_loop:none
  /// CHECK-DAG: <<Store2:i\d+>>    Add [{{i\d+}},<<Const2>>]                  loop:<<Loop>>      outer_loop:none
  /// CHECK-DAG:                    ArraySet [<<Param>>,<<Const0>>,<<Store2>>] loop:<<Loop>>      outer_loop:none

  /// CHECK-START: int Main.$noinline$deadAddAfterUnrollingAndSimplification(int[]) dead_code_elimination$before_codegen (before)
  /// CHECK:                        Add
  /// CHECK:                        Add
  /// CHECK:                        Add
  /// CHECK:                        Add
  /// CHECK-NOT:                    Add

  /// CHECK-START: int Main.$noinline$deadAddAfterUnrollingAndSimplification(int[]) dead_code_elimination$before_codegen (after)
  /// CHECK-DAG: <<Param:l\d+>>     ParameterValue                             loop:none
  /// CHECK-DAG: <<Const0:i\d+>>    IntConstant 0                              loop:none
  /// CHECK-DAG: <<Const2:i\d+>>    IntConstant 2                              loop:none
  /// CHECK-DAG: <<IndexPhi:i\d+>>  Phi [<<Const0>>,{{i\d+}}]                  loop:<<Loop:B\d+>> outer_loop:none
  //            Induction variable:
  /// CHECK-DAG:                    Add [<<IndexPhi>>,<<Const2>>]              loop:<<Loop>>      outer_loop:none
  //            Array Element Addition:
  /// CHECK-DAG: <<Store:i\d+>>     Add [{{i\d+}},<<Const2>>]                  loop:<<Loop>>      outer_loop:none
  /// CHECK-DAG:                    ArraySet [<<Param>>,<<Const0>>,<<Store>>] loop:<<Loop>>      outer_loop:none

  /// CHECK-START: int Main.$noinline$deadAddAfterUnrollingAndSimplification(int[]) dead_code_elimination$before_codegen (after)
  /// CHECK:                        Add
  /// CHECK:                        Add
  /// CHECK-NOT:                    Add
  public static int $noinline$deadAddAfterUnrollingAndSimplification(int[] array) {
    for (int i = 0; i < 50; ++i) {
        // Array access prevents transformation to closed-form expression
        array[0]++;
    }
    return array[0];
  }

  // If a == b returns b (which is equal to a) else returns a. This can be simplified to just
  // return a.

  /// CHECK-START: int Main.$noinline$returnSecondIfEqualElseFirstInt(int, int) instruction_simplifier$after_gvn (before)
  /// CHECK:     <<Param1:i\d+>> ParameterValue
  /// CHECK:     <<Param2:i\d+>> ParameterValue
  /// CHECK:     <<Select:i\d+>> Select [<<Param2>>,<<Param1>>,<<Cond:z\d+>>]
  /// CHECK:     <<Return:v\d+>> Return [<<Select>>]

  /// CHECK-START: int Main.$noinline$returnSecondIfEqualElseFirstInt(int, int) instruction_simplifier$after_gvn (after)
  /// CHECK:     <<Param1:i\d+>> ParameterValue
  /// CHECK:     <<Param2:i\d+>> ParameterValue
  /// CHECK:     <<Return:v\d+>> Return [<<Param1>>]

  /// CHECK-START: int Main.$noinline$returnSecondIfEqualElseFirstInt(int, int) instruction_simplifier$after_gvn (after)
  /// CHECK-NOT: Select
  private static int $noinline$returnSecondIfEqualElseFirstInt(int a, int b) {
    return a == b ? b : a;
  }

  /// CHECK-START: long Main.$noinline$returnSecondIfEqualElseFirstLong(long, long) instruction_simplifier$after_gvn (before)
  /// CHECK:     <<Param1:j\d+>> ParameterValue
  /// CHECK:     <<Param2:j\d+>> ParameterValue
  /// CHECK:     <<Select:j\d+>> Select [<<Param2>>,<<Param1>>,<<Cond:z\d+>>]
  /// CHECK:     <<Return:v\d+>> Return [<<Select>>]

  /// CHECK-START: long Main.$noinline$returnSecondIfEqualElseFirstLong(long, long) instruction_simplifier$after_gvn (after)
  /// CHECK:     <<Param1:j\d+>> ParameterValue
  /// CHECK:     <<Param2:j\d+>> ParameterValue
  /// CHECK:     <<Return:v\d+>> Return [<<Param1>>]

  /// CHECK-START: long Main.$noinline$returnSecondIfEqualElseFirstLong(long, long) instruction_simplifier$after_gvn (after)
  /// CHECK-NOT: Select
  private static long $noinline$returnSecondIfEqualElseFirstLong(long a, long b) {
    return a == b ? b : a;
  }

  // Note that we do not do the optimization for Float/Double.

  /// CHECK-START: float Main.$noinline$returnSecondIfEqualElseFirstFloat(float, float) instruction_simplifier$after_gvn (before)
  /// CHECK:     <<Param1:f\d+>> ParameterValue
  /// CHECK:     <<Param2:f\d+>> ParameterValue
  /// CHECK:     <<Select:f\d+>> Select [<<Param2>>,<<Param1>>,<<Cond:z\d+>>]
  /// CHECK:     <<Return:v\d+>> Return [<<Select>>]

  /// CHECK-START: float Main.$noinline$returnSecondIfEqualElseFirstFloat(float, float) disassembly (after)
  /// CHECK:     <<Param1:f\d+>> ParameterValue
  /// CHECK:     <<Param2:f\d+>> ParameterValue
  /// CHECK:     <<Select:f\d+>> Select [<<Param2>>,<<Param1>>,<<Cond:z\d+>>]
  /// CHECK:     <<Return:v\d+>> Return [<<Select>>]
  private static float $noinline$returnSecondIfEqualElseFirstFloat(float a, float b) {
    return a == b ? b : a;
  }

  /// CHECK-START: double Main.$noinline$returnSecondIfEqualElseFirstDouble(double, double) instruction_simplifier$after_gvn (before)
  /// CHECK:     <<Param1:d\d+>> ParameterValue
  /// CHECK:     <<Param2:d\d+>> ParameterValue
  /// CHECK:     <<Select:d\d+>> Select [<<Param2>>,<<Param1>>,<<Cond:z\d+>>]
  /// CHECK:     <<Return:v\d+>> Return [<<Select>>]

  /// CHECK-START: double Main.$noinline$returnSecondIfEqualElseFirstDouble(double, double) disassembly (after)
  /// CHECK:     <<Param1:d\d+>> ParameterValue
  /// CHECK:     <<Param2:d\d+>> ParameterValue
  /// CHECK:     <<Select:d\d+>> Select [<<Param2>>,<<Param1>>,<<Cond:z\d+>>]
  /// CHECK:     <<Return:v\d+>> Return [<<Select>>]
  private static double $noinline$returnSecondIfEqualElseFirstDouble(double a, double b) {
    return a == b ? b : a;
  }

  // If a != b returns b else returns a (which is equal to b). This can be simplified to just
  // return b.

  /// CHECK-START: int Main.$noinline$returnSecondIfNotEqualElseFirstInt(int, int) instruction_simplifier$after_gvn (before)
  /// CHECK:     <<Param1:i\d+>> ParameterValue
  /// CHECK:     <<Param2:i\d+>> ParameterValue
  /// CHECK:     <<Select:i\d+>> Select [<<Param2>>,<<Param1>>,<<Cond:z\d+>>]
  /// CHECK:     <<Return:v\d+>> Return [<<Select>>]

  /// CHECK-START: int Main.$noinline$returnSecondIfNotEqualElseFirstInt(int, int) instruction_simplifier$after_gvn (after)
  /// CHECK:     <<Param1:i\d+>> ParameterValue
  /// CHECK:     <<Param2:i\d+>> ParameterValue
  /// CHECK:     <<Return:v\d+>> Return [<<Param2>>]

  /// CHECK-START: int Main.$noinline$returnSecondIfNotEqualElseFirstInt(int, int) instruction_simplifier$after_gvn (after)
  /// CHECK-NOT: Select
  private static int $noinline$returnSecondIfNotEqualElseFirstInt(int a, int b) {
    return a != b ? b : a;
  }

  /// CHECK-START: long Main.$noinline$returnSecondIfNotEqualElseFirstLong(long, long) instruction_simplifier$after_gvn (before)
  /// CHECK:     <<Param1:j\d+>> ParameterValue
  /// CHECK:     <<Param2:j\d+>> ParameterValue
  /// CHECK:     <<Select:j\d+>> Select [<<Param2>>,<<Param1>>,<<Cond:z\d+>>]
  /// CHECK:     <<Return:v\d+>> Return [<<Select>>]

  /// CHECK-START: long Main.$noinline$returnSecondIfNotEqualElseFirstLong(long, long) instruction_simplifier$after_gvn (after)
  /// CHECK:     <<Param1:j\d+>> ParameterValue
  /// CHECK:     <<Param2:j\d+>> ParameterValue
  /// CHECK:     <<Return:v\d+>> Return [<<Param2>>]

  /// CHECK-START: long Main.$noinline$returnSecondIfNotEqualElseFirstLong(long, long) instruction_simplifier$after_gvn (after)
  /// CHECK-NOT: Select
  private static long $noinline$returnSecondIfNotEqualElseFirstLong(long a, long b) {
    return a != b ? b : a;
  }

  // Note that we do not do the optimization for Float/Double.

  /// CHECK-START: float Main.$noinline$returnSecondIfNotEqualElseFirstFloat(float, float) instruction_simplifier$after_gvn (before)
  /// CHECK:     <<Param1:f\d+>> ParameterValue
  /// CHECK:     <<Param2:f\d+>> ParameterValue
  /// CHECK:     <<Select:f\d+>> Select [<<Param2>>,<<Param1>>,<<Cond:z\d+>>]
  /// CHECK:     <<Return:v\d+>> Return [<<Select>>]

  /// CHECK-START: float Main.$noinline$returnSecondIfNotEqualElseFirstFloat(float, float) disassembly (after)
  /// CHECK:     <<Param1:f\d+>> ParameterValue
  /// CHECK:     <<Param2:f\d+>> ParameterValue
  /// CHECK:     <<Select:f\d+>> Select [<<Param2>>,<<Param1>>,<<Cond:z\d+>>]
  /// CHECK:     <<Return:v\d+>> Return [<<Select>>]
  private static float $noinline$returnSecondIfNotEqualElseFirstFloat(float a, float b) {
    return a != b ? b : a;
  }

  /// CHECK-START: double Main.$noinline$returnSecondIfNotEqualElseFirstDouble(double, double) instruction_simplifier$after_gvn (before)
  /// CHECK:     <<Param1:d\d+>> ParameterValue
  /// CHECK:     <<Param2:d\d+>> ParameterValue
  /// CHECK:     <<Select:d\d+>> Select [<<Param2>>,<<Param1>>,<<Cond:z\d+>>]
  /// CHECK:     <<Return:v\d+>> Return [<<Select>>]

  /// CHECK-START: double Main.$noinline$returnSecondIfNotEqualElseFirstDouble(double, double) disassembly (after)
  /// CHECK:     <<Param1:d\d+>> ParameterValue
  /// CHECK:     <<Param2:d\d+>> ParameterValue
  /// CHECK:     <<Select:d\d+>> Select [<<Param2>>,<<Param1>>,<<Cond:z\d+>>]
  /// CHECK:     <<Return:v\d+>> Return [<<Select>>]
  private static double $noinline$returnSecondIfNotEqualElseFirstDouble(double a, double b) {
    return a != b ? b : a;
  }

  // Check that (x << N >>> N) and (x << N >> N) are simplified to corresponding TypeConversion.

  // Check T -> int -> Unsigned<T> -> int cases.

  /// CHECK-START: int Main.$noinline$testByteToIntAsUnsigned(byte) instruction_simplifier (before)
  /// CHECK:     <<Param:b\d+>>   ParameterValue
  /// CHECK:     <<Const24:i\d+>> IntConstant 24
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const24>>]
  /// CHECK:     <<UShr:i\d+>>    UShr [<<Shl>>,<<Const24>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<UShr>>]

  /// CHECK-START: int Main.$noinline$testByteToIntAsUnsigned(byte) instruction_simplifier (after)
  /// CHECK:     <<Param:b\d+>>   ParameterValue
  /// CHECK:     <<Conv:a\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testByteToIntAsUnsigned(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testByteToIntAsUnsigned(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  UShr
  private static int $noinline$testByteToIntAsUnsigned(byte arg) {
    return arg << 24 >>> 24;
  }

  /// CHECK-START: int Main.$noinline$testShortToIntAsUnsigned(short) instruction_simplifier (before)
  /// CHECK:     <<Param:s\d+>>   ParameterValue
  /// CHECK:     <<Const16:i\d+>> IntConstant 16
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const16>>]
  /// CHECK:     <<UShr:i\d+>>    UShr [<<Shl>>,<<Const16>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<UShr>>]

  /// CHECK-START: int Main.$noinline$testShortToIntAsUnsigned(short) instruction_simplifier (after)
  /// CHECK:     <<Param:s\d+>>   ParameterValue
  /// CHECK:     <<Conv:c\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testShortToIntAsUnsigned(short) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testShortToIntAsUnsigned(short) instruction_simplifier (after)
  /// CHECK-NOT:                  UShr
  private static int $noinline$testShortToIntAsUnsigned(short arg) {
    return arg << 16 >>> 16;
  }

  /// CHECK-START: int Main.$noinline$testCharToIntAsUnsigned(char) instruction_simplifier (before)
  /// CHECK:     <<Param:c\d+>>   ParameterValue
  /// CHECK:     <<Const16:i\d+>> IntConstant 16
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const16>>]
  /// CHECK:     <<UShr:i\d+>>    UShr [<<Shl>>,<<Const16>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<UShr>>]

  /// CHECK-START: int Main.$noinline$testCharToIntAsUnsigned(char) instruction_simplifier (after)
  /// CHECK:     <<Param:c\d+>>   ParameterValue
  /// CHECK:     <<Return:v\d+>>  Return [<<Param>>]

  /// CHECK-START: int Main.$noinline$testCharToIntAsUnsigned(char) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testCharToIntAsUnsigned(char) instruction_simplifier (after)
  /// CHECK-NOT:                  UShr
  private static int $noinline$testCharToIntAsUnsigned(char arg) {
    return arg << 16 >>> 16;
  }

  // Check T -> int -> Signed<T> -> int cases.

  /// CHECK-START: int Main.$noinline$testByteToIntAsSigned(byte) instruction_simplifier (before)
  /// CHECK:     <<Param:b\d+>>   ParameterValue
  /// CHECK:     <<Const24:i\d+>> IntConstant 24
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const24>>]
  /// CHECK:     <<Shr:i\d+>>     Shr [<<Shl>>,<<Const24>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Shr>>]

  /// CHECK-START: int Main.$noinline$testByteToIntAsSigned(byte) instruction_simplifier (after)
  /// CHECK:     <<Param:b\d+>>   ParameterValue
  /// CHECK:     <<Return:v\d+>>  Return [<<Param>>]

  /// CHECK-START: int Main.$noinline$testByteToIntAsSigned(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testByteToIntAsSigned(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  Shr
  private static int $noinline$testByteToIntAsSigned(byte arg) {
    return arg << 24 >> 24;
  }

  /// CHECK-START: int Main.$noinline$testShortToIntAsSigned(short) instruction_simplifier (before)
  /// CHECK:     <<Param:s\d+>>   ParameterValue
  /// CHECK:     <<Const16:i\d+>> IntConstant 16
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const16>>]
  /// CHECK:     <<Shr:i\d+>>     Shr [<<Shl>>,<<Const16>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Shr>>]

  /// CHECK-START: int Main.$noinline$testShortToIntAsSigned(short) instruction_simplifier (after)
  /// CHECK:     <<Param:s\d+>>   ParameterValue
  /// CHECK:     <<Return:v\d+>>  Return [<<Param>>]

  /// CHECK-START: int Main.$noinline$testShortToIntAsSigned(short) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testShortToIntAsSigned(short) instruction_simplifier (after)
  /// CHECK-NOT:                  Shr
  private static int $noinline$testShortToIntAsSigned(short arg) {
    return arg << 16 >> 16;
  }

  /// CHECK-START: int Main.$noinline$testCharToIntAsSigned(char) instruction_simplifier (before)
  /// CHECK:     <<Param:c\d+>>   ParameterValue
  /// CHECK:     <<Const16:i\d+>> IntConstant 16
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const16>>]
  /// CHECK:     <<Shr:i\d+>>     Shr [<<Shl>>,<<Const16>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Shr>>]

  /// CHECK-START: int Main.$noinline$testCharToIntAsSigned(char) instruction_simplifier (after)
  /// CHECK:     <<Param:c\d+>>   ParameterValue
  /// CHECK:     <<Conv:s\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testCharToIntAsSigned(char) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testCharToIntAsSigned(char) instruction_simplifier (after)
  /// CHECK-NOT:                  Shr
  private static int $noinline$testCharToIntAsSigned(char arg) {
    return arg << 16 >> 16;
  }

  // Check T -> U (narrowing) -> int cases where M is unsigned type.

  /// CHECK-START: int Main.$noinline$testShortToByteToIntAsUnsigned(short) instruction_simplifier (before)
  /// CHECK:     <<Param:s\d+>>   ParameterValue
  /// CHECK:     <<Const24:i\d+>> IntConstant 24
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const24>>]
  /// CHECK:     <<UShr:i\d+>>    UShr [<<Shl>>,<<Const24>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<UShr>>]

  /// CHECK-START: int Main.$noinline$testShortToByteToIntAsUnsigned(short) instruction_simplifier (after)
  /// CHECK:     <<Param:s\d+>>   ParameterValue
  /// CHECK:     <<Conv:a\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testShortToByteToIntAsUnsigned(short) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testShortToByteToIntAsUnsigned(short) instruction_simplifier (after)
  /// CHECK-NOT:                  UShr
  private static int $noinline$testShortToByteToIntAsUnsigned(short arg) {
    return arg << 24 >>> 24;
  }

  /// CHECK-START: int Main.$noinline$testCharToByteToIntAsUnsigned(char) instruction_simplifier (before)
  /// CHECK:     <<Param:c\d+>>   ParameterValue
  /// CHECK:     <<Const24:i\d+>> IntConstant 24
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const24>>]
  /// CHECK:     <<UShr:i\d+>>    UShr [<<Shl>>,<<Const24>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<UShr>>]

  /// CHECK-START: int Main.$noinline$testCharToByteToIntAsUnsigned(char) instruction_simplifier (after)
  /// CHECK:     <<Param:c\d+>>   ParameterValue
  /// CHECK:     <<Conv:a\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testCharToByteToIntAsUnsigned(char) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testCharToByteToIntAsUnsigned(char) instruction_simplifier (after)
  /// CHECK-NOT:                  UShr
  private static int $noinline$testCharToByteToIntAsUnsigned(char arg) {
    return arg << 24 >>> 24;
  }

  // Check T -> S (narrowing) -> int cases where S is signed type.

  /// CHECK-START: int Main.$noinline$testShortToByteToIntAsSigned(short) instruction_simplifier (before)
  /// CHECK:     <<Param:s\d+>>   ParameterValue
  /// CHECK:     <<Const24:i\d+>> IntConstant 24
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const24>>]
  /// CHECK:     <<Shr:i\d+>>     Shr [<<Shl>>,<<Const24>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Shr>>]

  /// CHECK-START: int Main.$noinline$testShortToByteToIntAsSigned(short) instruction_simplifier (after)
  /// CHECK:     <<Param:s\d+>>   ParameterValue
  /// CHECK:     <<Conv:b\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testShortToByteToIntAsSigned(short) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testShortToByteToIntAsSigned(short) instruction_simplifier (after)
  /// CHECK-NOT:                  Shr
  private static int $noinline$testShortToByteToIntAsSigned(short arg) {
    return arg << 24 >> 24;
  }

  /// CHECK-START: int Main.$noinline$testCharToByteToIntAsSigned(char) instruction_simplifier (before)
  /// CHECK:     <<Param:c\d+>>   ParameterValue
  /// CHECK:     <<Const24:i\d+>> IntConstant 24
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const24>>]
  /// CHECK:     <<Shr:i\d+>>     Shr [<<Shl>>,<<Const24>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Shr>>]

  /// CHECK-START: int Main.$noinline$testCharToByteToIntAsSigned(char) instruction_simplifier (after)
  /// CHECK:     <<Param:c\d+>>   ParameterValue
  /// CHECK:     <<Conv:b\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testCharToByteToIntAsSigned(char) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testCharToByteToIntAsSigned(char) instruction_simplifier (after)
  /// CHECK-NOT:                  Shr
  private static int $noinline$testCharToByteToIntAsSigned(char arg) {
    return arg << 24 >> 24;
  }

  // Check int -> U -> int cases where U is a unsigned type.

  /// CHECK-START: int Main.$noinline$testIntToUnsignedByteToInt(int) instruction_simplifier (before)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Const24:i\d+>> IntConstant 24
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const24>>]
  /// CHECK:     <<UShr:i\d+>>    UShr [<<Shl>>,<<Const24>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<UShr>>]

  /// CHECK-START: int Main.$noinline$testIntToUnsignedByteToInt(int) instruction_simplifier (after)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Conv:a\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testIntToUnsignedByteToInt(int) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testIntToUnsignedByteToInt(int) instruction_simplifier (after)
  /// CHECK-NOT:                  UShr
  private static int $noinline$testIntToUnsignedByteToInt(int arg) {
    return arg << 24 >>> 24;
  }

  /// CHECK-START: int Main.$noinline$testIntToUnsignedShortToInt(int) instruction_simplifier (before)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Const16:i\d+>> IntConstant 16
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const16>>]
  /// CHECK:     <<UShr:i\d+>>    UShr [<<Shl>>,<<Const16>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<UShr>>]

  /// CHECK-START: int Main.$noinline$testIntToUnsignedShortToInt(int) instruction_simplifier (after)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Conv:c\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testIntToUnsignedShortToInt(int) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testIntToUnsignedShortToInt(int) instruction_simplifier (after)
  /// CHECK-NOT:                  UShr
  private static int $noinline$testIntToUnsignedShortToInt(int arg) {
    return arg << 16 >>> 16;
  }

  // Check int -> S -> int cases where S is a signed type.

  /// CHECK-START: int Main.$noinline$testIntToSignedByteToInt(int) instruction_simplifier (before)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Const24:i\d+>> IntConstant 24
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const24>>]
  /// CHECK:     <<Shr:i\d+>>     Shr [<<Shl>>,<<Const24>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Shr>>]

  /// CHECK-START: int Main.$noinline$testIntToSignedByteToInt(int) instruction_simplifier (after)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Conv:b\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testIntToSignedByteToInt(int) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testIntToSignedByteToInt(int) instruction_simplifier (after)
  /// CHECK-NOT:                  Shr
  private static int $noinline$testIntToSignedByteToInt(int arg) {
    return arg << 24 >> 24;
  }

  /// CHECK-START: int Main.$noinline$testIntToSignedShortToInt(int) instruction_simplifier (before)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Const16:i\d+>> IntConstant 16
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const16>>]
  /// CHECK:     <<Shr:i\d+>>     Shr [<<Shl>>,<<Const16>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Shr>>]

  /// CHECK-START: int Main.$noinline$testIntToSignedShortToInt(int) instruction_simplifier (after)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Conv:s\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testIntToSignedShortToInt(int) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testIntToSignedShortToInt(int) instruction_simplifier (after)
  /// CHECK-NOT:                  Shr
  private static int $noinline$testIntToSignedShortToInt(int arg) {
    return arg << 16 >> 16;
  }

  // Check T -> U -> int cases where U is a unsigned type.

  /// CHECK-START: int Main.$noinline$testCharToUnsignedByteToInt(char) instruction_simplifier (before)
  /// CHECK:     <<Param:c\d+>>   ParameterValue
  /// CHECK:     <<Const24:i\d+>> IntConstant 24
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const24>>]
  /// CHECK:     <<UShr:i\d+>>    UShr [<<Shl>>,<<Const24>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<UShr>>]

  /// CHECK-START: int Main.$noinline$testCharToUnsignedByteToInt(char) instruction_simplifier (after)
  /// CHECK:     <<Param:c\d+>>   ParameterValue
  /// CHECK:     <<Conv:a\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testCharToUnsignedByteToInt(char) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testCharToUnsignedByteToInt(char) instruction_simplifier (after)
  /// CHECK-NOT:                  UShr
  private static int $noinline$testCharToUnsignedByteToInt(char arg) {
    return arg << 24 >>> 24;
  }

  /// CHECK-START: int Main.$noinline$testShortToUnsignedByteToInt(short) instruction_simplifier (before)
  /// CHECK:     <<Param:s\d+>>   ParameterValue
  /// CHECK:     <<Const24:i\d+>> IntConstant 24
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const24>>]
  /// CHECK:     <<UShr:i\d+>>    UShr [<<Shl>>,<<Const24>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<UShr>>]

  /// CHECK-START: int Main.$noinline$testShortToUnsignedByteToInt(short) instruction_simplifier (after)
  /// CHECK:     <<Param:s\d+>>   ParameterValue
  /// CHECK:     <<Conv:a\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testShortToUnsignedByteToInt(short) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testShortToUnsignedByteToInt(short) instruction_simplifier (after)
  /// CHECK-NOT:                  UShr
  private static int $noinline$testShortToUnsignedByteToInt(short arg) {
    return arg << 24 >>> 24;
  }

  // Check T -> S -> int cases where S is a signed type.

  /// CHECK-START: int Main.$noinline$testCharToSignedByteToInt(char) instruction_simplifier (before)
  /// CHECK:     <<Param:c\d+>>   ParameterValue
  /// CHECK:     <<Const24:i\d+>> IntConstant 24
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const24>>]
  /// CHECK:     <<Shr:i\d+>>     Shr [<<Shl>>,<<Const24>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Shr>>]

  /// CHECK-START: int Main.$noinline$testCharToSignedByteToInt(char) instruction_simplifier (after)
  /// CHECK:     <<Param:c\d+>>   ParameterValue
  /// CHECK:     <<Conv:b\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testCharToSignedByteToInt(char) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testCharToSignedByteToInt(char) instruction_simplifier (after)
  /// CHECK-NOT:                  Shr
  private static int $noinline$testCharToSignedByteToInt(char arg) {
    return arg << 24 >> 24;
  }

  /// CHECK-START: int Main.$noinline$testShortToSignedByteToInt(short) instruction_simplifier (before)
  /// CHECK:     <<Param:s\d+>>   ParameterValue
  /// CHECK:     <<Const24:i\d+>> IntConstant 24
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const24>>]
  /// CHECK:     <<Shr:i\d+>>     Shr [<<Shl>>,<<Const24>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Shr>>]

  /// CHECK-START: int Main.$noinline$testShortToSignedByteToInt(short) instruction_simplifier (after)
  /// CHECK:     <<Param:s\d+>>   ParameterValue
  /// CHECK:     <<Conv:b\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testShortToSignedByteToInt(short) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testShortToSignedByteToInt(short) instruction_simplifier (after)
  /// CHECK-NOT:                  Shr
  private static int $noinline$testShortToSignedByteToInt(short arg) {
    return arg << 24 >> 24;
  }

  // Check cases with shift amounts > 32.

  /// CHECK-START: int Main.$noinline$testUnsignedPromotionWithHugeShiftAmount(int) instruction_simplifier (before)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Const48:i\d+>> IntConstant 48
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const48>>]
  /// CHECK:     <<UShr:i\d+>>    UShr [<<Shl>>,<<Const48>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<UShr>>]

  /// CHECK-START: int Main.$noinline$testUnsignedPromotionWithHugeShiftAmount(int) instruction_simplifier (after)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Conv:c\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testUnsignedPromotionWithHugeShiftAmount(int) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testUnsignedPromotionWithHugeShiftAmount(int) instruction_simplifier (after)
  /// CHECK-NOT:                  UShr
  private static int $noinline$testUnsignedPromotionWithHugeShiftAmount(int arg) {
    return arg << 48 >>> 48;
  }

  /// CHECK-START: int Main.$noinline$testUnsignedPromotionWithHugeMismatchedShiftAmount(int) instruction_simplifier (before)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Const48:i\d+>> IntConstant 48
  /// CHECK:     <<Const16:i\d+>> IntConstant 16
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const48>>]
  /// CHECK:     <<UShr:i\d+>>    UShr [<<Shl>>,<<Const16>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<UShr>>]

  /// CHECK-START: int Main.$noinline$testUnsignedPromotionWithHugeMismatchedShiftAmount(int) instruction_simplifier (after)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Conv:c\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testUnsignedPromotionWithHugeMismatchedShiftAmount(int) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testUnsignedPromotionWithHugeMismatchedShiftAmount(int) instruction_simplifier (after)
  /// CHECK-NOT:                  UShr
  private static int $noinline$testUnsignedPromotionWithHugeMismatchedShiftAmount(int arg) {
    return arg << 48 >>> 16;
  }

  // Negative checks.

  /// CHECK-START: long Main.$noinline$testUnsignedPromotionToLong(long) instruction_simplifier (after)
  /// CHECK:     <<Param:j\d+>>   ParameterValue
  /// CHECK:     <<Const56:i\d+>> IntConstant 56
  /// CHECK:     <<Shl:j\d+>>     Shl [<<Param>>,<<Const56>>]
  /// CHECK:     <<UShr:j\d+>>    UShr [<<Shl>>,<<Const56>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<UShr>>]
  private static long $noinline$testUnsignedPromotionToLong(long arg) {
    // Check that we don't do simplification in the case of unsigned promotion to long.
    return arg << 56 >>> 56;
  }

  /// CHECK-START: long Main.$noinline$testSignedPromotionToLong(long) instruction_simplifier (after)
  /// CHECK:     <<Param:j\d+>>   ParameterValue
  /// CHECK:     <<Const56:i\d+>> IntConstant 56
  /// CHECK:     <<Shl:j\d+>>     Shl [<<Param>>,<<Const56>>]
  /// CHECK:     <<Shr:j\d+>>     Shr [<<Shl>>,<<Const56>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Shr>>]
  private static long $noinline$testSignedPromotionToLong(long arg) {
    // Check that we don't do simplification in the case of signed promotion to long.
    return arg << 56 >> 56;
  }

  /// CHECK-START: int Main.$noinline$testUnsignedPromotionWithNonConstantShiftAmount(int, int) instruction_simplifier (after)
  /// CHECK:     <<Param1:i\d+>>  ParameterValue
  /// CHECK:     <<Param2:i\d+>>  ParameterValue
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param1>>,<<Param2>>]
  /// CHECK:     <<UShr:i\d+>>    UShr [<<Shl>>,<<Param2>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<UShr>>]
  private static int
  $noinline$testUnsignedPromotionWithNonConstantShiftAmount(int value, int shift_amount) {
    // Check that we don't do simplification in the case of unsigned promotion with
    // non constant shift amount.
    return value << shift_amount >>> shift_amount;
  }

  /// CHECK-START: int Main.$noinline$testSignedPromotionWithNonConstantShiftAmount(int, int) instruction_simplifier (after)
  /// CHECK:     <<Param1:i\d+>>  ParameterValue
  /// CHECK:     <<Param2:i\d+>>  ParameterValue
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param1>>,<<Param2>>]
  /// CHECK:     <<Shr:i\d+>>     Shr [<<Shl>>,<<Param2>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Shr>>]
  private static int
  $noinline$testSignedPromotionWithNonConstantShiftAmount(int value, int shift_amount) {
    // Check that we don't do simplification in the case of signed promotion with
    // non constant shift amount.
    return value << shift_amount >> shift_amount;
  }

  /// CHECK-START: int Main.$noinline$testUnsignedPromotionWithShlUse(int) instruction_simplifier (after)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Const24:i\d+>> IntConstant 24
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const24>>]
  /// CHECK:     <<UShr:i\d+>>    UShr [<<Shl>>,<<Const24>>]
  /// CHECK:     <<Add:i\d+>>     Add [<<UShr>>,<<Shl>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Add>>]
  private static int $noinline$testUnsignedPromotionWithShlUse(int arg) {
    // Check that we don't do simplification in the case of unsigned promotion
    // with shl instruction that has more than 1 user.
    int tmp = arg << 24;
    return (tmp >>> 24) + tmp;
  }

  /// CHECK-START: int Main.$noinline$testSignedPromotionWithShlUse(int) instruction_simplifier (after)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Const24:i\d+>> IntConstant 24
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const24>>]
  /// CHECK:     <<Shr:i\d+>>     Shr [<<Shl>>,<<Const24>>]
  /// CHECK:     <<Add:i\d+>>     Add [<<Shr>>,<<Shl>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Add>>]
  private static int $noinline$testSignedPromotionWithShlUse(int arg) {
    // Check that we don't do simplification in the case of signed promotion
    // with shl instruction that has more than 1 user.
    int tmp = arg << 24;
    return (tmp >> 24) + tmp;
  }

  /// CHECK-START: int Main.$noinline$testUnsignedPromotionPatternWithIncorrectShiftAmountConstant(int) instruction_simplifier (after)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Const25:i\d+>> IntConstant 25
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const25>>]
  /// CHECK:     <<UShr:i\d+>>    UShr [<<Shl>>,<<Const25>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<UShr>>]

  private static int
  $noinline$testUnsignedPromotionPatternWithIncorrectShiftAmountConstant(int arg) {
    // Check that we don't do simplification in the case of 32 - N doesn't correspond
    // to the size of an integral type.
    return arg << 25 >>> 25;
  }

  /// CHECK-START: int Main.$noinline$testSignedPromotionPatternWithIncorrectShiftAmountConstant(int) instruction_simplifier (after)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Const25:i\d+>> IntConstant 25
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const25>>]
  /// CHECK:     <<Shr:i\d+>>     Shr [<<Shl>>,<<Const25>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Shr>>]

  private static int
  $noinline$testSignedPromotionPatternWithIncorrectShiftAmountConstant(int arg) {
    // Check that we don't do simplification in the case of 32 - N doesn't correspond
    // to the size of an integral type.
    return arg << 25 >> 25;
  }

  /// CHECK-START: int Main.$noinline$testUnsignedPromotionPatternWithDifferentShiftAmountConstants(int) instruction_simplifier (after)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Const24:i\d+>> IntConstant 24
  /// CHECK:     <<Const16:i\d+>> IntConstant 16
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const24>>]
  /// CHECK:     <<UShr:i\d+>>    UShr [<<Shl>>,<<Const16>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<UShr>>]

  private static int
  $noinline$testUnsignedPromotionPatternWithDifferentShiftAmountConstants(int arg) {
    // Check that we don't do simplification in the case of different shift amounts.
    return arg << 24 >>> 16;
  }

  /// CHECK-START: int Main.$noinline$testSignedPromotionPatternWithDifferentShiftAmountConstants(int) instruction_simplifier (after)
  /// CHECK:     <<Param:i\d+>>   ParameterValue
  /// CHECK:     <<Const25:i\d+>> IntConstant 25
  /// CHECK:     <<Const26:i\d+>> IntConstant 26
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const25>>]
  /// CHECK:     <<Shr:i\d+>>     Shr [<<Shl>>,<<Const26>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Shr>>]

  private static
  int $noinline$testSignedPromotionPatternWithDifferentShiftAmountConstants(int arg) {
    // Check that we do not simplification in the case of different shift amounts.
    return arg << 25 >> 26;
  }

  // Check that we don't introduce new implicit type conversions so the following pattern
  // does not occur in the graph:
  //
  // <<ImplicitConv>> TypeConversion
  // <<ExplicitConv>> TypeConversonn [<<ImplicitConv>>]
  //
  // That will lead to a crash because InstructionSimplifier removes implicit type conversions
  // and during visiting TypeConversion instruction expects that its inputs have been already
  // simplified.
  //
  // The structure of the following tests is
  //
  //   (T) ((x << N) >> N) or (T) ((x << N) >>> N)
  //
  // where
  //   * K is a type of x
  //   * Shifts correspond to implicit type conversion K -> M
  //   * M -> T conversion is explicit
  //
  // T itself doesn't matter, the only important thing is that M -> T is explicit.
  //
  // We check cases when shifts correspond to the following implicit type conversions:
  //   byte -> byte
  //   byte -> short
  //   unsigned byte -> unsigned byte
  //   unsigned byte -> short
  //   unsigned byte -> char
  //   short -> short
  //   char -> char
  //
  // To produce unsigned byte bitwise AND with 0xFF is used.

  /// CHECK-START: int Main.$noinline$testByteToByteToChar(byte) instruction_simplifier (before)
  /// CHECK:     <<Param:b\d+>>   ParameterValue
  /// CHECK:     <<Const24:i\d+>> IntConstant 24
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const24>>]
  /// CHECK:     <<Shr:i\d+>>     Shr [<<Shl>>,<<Const24>>]
  /// CHECK:     <<Conv:c\d+>>    TypeConversion [<<Shr>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testByteToByteToChar(byte) instruction_simplifier (after)
  /// CHECK:     <<Param:b\d+>>   ParameterValue
  /// CHECK:     <<Conv:c\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testByteToByteToChar(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testByteToByteToChar(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  Shr
  private static int $noinline$testByteToByteToChar(byte arg) {
    return (char) ((arg << 24) >> 24);
  }

  /// CHECK-START: int Main.$noinline$testByteToShortToByte(byte) instruction_simplifier (before)
  /// CHECK:     <<Param:b\d+>>   ParameterValue
  /// CHECK:     <<Const16:i\d+>> IntConstant 16
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const16>>]
  /// CHECK:     <<Shr:i\d+>>     Shr [<<Shl>>,<<Const16>>]
  /// CHECK:     <<Conv:b\d+>>    TypeConversion [<<Shr>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testByteToShortToByte(byte) instruction_simplifier (after)
  /// CHECK:     <<Param:b\d+>>   ParameterValue
  /// CHECK:     <<Return:v\d+>>  Return [<<Param>>]

  /// CHECK-START: int Main.$noinline$testByteToShortToByte(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testByteToShortToByte(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  Shr

  /// CHECK-START: int Main.$noinline$testByteToShortToByte(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  TypeConversion
  private static int $noinline$testByteToShortToByte(byte arg) {
    return (byte) ((arg << 16) >> 16);
  }

  /// CHECK-START: int Main.$noinline$testUnsignedByteToUnsignedByteToByte(byte) instruction_simplifier (before)
  /// CHECK:     <<Param:b\d+>>    ParameterValue
  /// CHECK:     <<Const255:i\d+>> IntConstant 255
  /// CHECK:     <<Const24:i\d+>>  IntConstant 24
  /// CHECK:     <<And:i\d+>>      And [<<Param>>,<<Const255>>]
  /// CHECK:     <<Shl:i\d+>>      Shl [<<And>>,<<Const24>>]
  /// CHECK:     <<UShr:i\d+>>     UShr [<<Shl>>,<<Const24>>]
  /// CHECK:     <<Conv:b\d+>>     TypeConversion [<<UShr>>]
  /// CHECK:     <<Return:v\d+>>   Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testUnsignedByteToUnsignedByteToByte(byte) instruction_simplifier (after)
  /// CHECK:     <<Param:b\d+>>   ParameterValue
  /// CHECK:     <<Return:v\d+>>  Return [<<Param>>]

  /// CHECK-START: int Main.$noinline$testUnsignedByteToUnsignedByteToByte(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testUnsignedByteToUnsignedByteToByte(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  Shr

  /// CHECK-START: int Main.$noinline$testUnsignedByteToUnsignedByteToByte(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  TypeConversion
  private static int $noinline$testUnsignedByteToUnsignedByteToByte(byte arg) {
    return (byte) (((arg & 0xFF) << 24) >>> 24);
  }

  /// CHECK-START: int Main.$noinline$testUnsignedByteToShortToByte(byte) instruction_simplifier (before)
  /// CHECK:     <<Param:b\d+>>    ParameterValue
  /// CHECK:     <<Const255:i\d+>> IntConstant 255
  /// CHECK:     <<Const16:i\d+>>  IntConstant 16
  /// CHECK:     <<And:i\d+>>      And [<<Param>>,<<Const255>>]
  /// CHECK:     <<Shl:i\d+>>      Shl [<<And>>,<<Const16>>]
  /// CHECK:     <<Shr:i\d+>>      Shr [<<Shl>>,<<Const16>>]
  /// CHECK:     <<Conv:b\d+>>     TypeConversion [<<Shr>>]
  /// CHECK:     <<Return:v\d+>>   Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testUnsignedByteToShortToByte(byte) instruction_simplifier (after)
  /// CHECK:     <<Param:b\d+>>   ParameterValue
  /// CHECK:     <<Return:v\d+>>  Return [<<Param>>]

  /// CHECK-START: int Main.$noinline$testUnsignedByteToShortToByte(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testUnsignedByteToShortToByte(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  Shr

  /// CHECK-START: int Main.$noinline$testUnsignedByteToShortToByte(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  TypeConversion
  private static int $noinline$testUnsignedByteToShortToByte(byte arg) {
    return (byte) (((arg & 0xFF) << 16) >> 16);
  }

  /// CHECK-START: int Main.$noinline$testUnsignedByteToCharToByte(byte) instruction_simplifier (before)
  /// CHECK:     <<Param:b\d+>>    ParameterValue
  /// CHECK:     <<Const255:i\d+>> IntConstant 255
  /// CHECK:     <<Const16:i\d+>>  IntConstant 16
  /// CHECK:     <<And:i\d+>>      And [<<Param>>,<<Const255>>]
  /// CHECK:     <<Shl:i\d+>>      Shl [<<And>>,<<Const16>>]
  /// CHECK:     <<UShr:i\d+>>     UShr [<<Shl>>,<<Const16>>]
  /// CHECK:     <<Conv:b\d+>>     TypeConversion [<<UShr>>]
  /// CHECK:     <<Return:v\d+>>   Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testUnsignedByteToCharToByte(byte) instruction_simplifier (after)
  /// CHECK:     <<Param:b\d+>>   ParameterValue
  /// CHECK:     <<Return:v\d+>>  Return [<<Param>>]

  /// CHECK-START: int Main.$noinline$testUnsignedByteToCharToByte(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testUnsignedByteToCharToByte(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  Shr

  /// CHECK-START: int Main.$noinline$testUnsignedByteToCharToByte(byte) instruction_simplifier (after)
  /// CHECK-NOT:                  TypeConversion
  private static int $noinline$testUnsignedByteToCharToByte(byte arg) {
    return (byte) (((arg & 0xFF) << 16) >>> 16);
  }

  /// CHECK-START: int Main.$noinline$testShortToShortToByte(short) instruction_simplifier (before)
  /// CHECK:     <<Param:s\d+>>   ParameterValue
  /// CHECK:     <<Const16:i\d+>> IntConstant 16
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const16>>]
  /// CHECK:     <<Shr:i\d+>>     Shr [<<Shl>>,<<Const16>>]
  /// CHECK:     <<Conv:b\d+>>    TypeConversion [<<Shr>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testShortToShortToByte(short) instruction_simplifier (after)
  /// CHECK:     <<Param:s\d+>>   ParameterValue
  /// CHECK:     <<Conv:b\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testShortToShortToByte(short) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testShortToShortToByte(short) instruction_simplifier (after)
  /// CHECK-NOT:                  Shr
  private static int $noinline$testShortToShortToByte(short arg) {
    return (byte) ((arg << 16) >> 16);
  }

  /// CHECK-START: int Main.$noinline$testCharToCharToByte(char) instruction_simplifier (before)
  /// CHECK:     <<Param:c\d+>>   ParameterValue
  /// CHECK:     <<Const16:i\d+>> IntConstant 16
  /// CHECK:     <<Shl:i\d+>>     Shl [<<Param>>,<<Const16>>]
  /// CHECK:     <<UShr:i\d+>>    UShr [<<Shl>>,<<Const16>>]
  /// CHECK:     <<Conv:b\d+>>    TypeConversion [<<UShr>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testCharToCharToByte(char) instruction_simplifier (after)
  /// CHECK:     <<Param:c\d+>>   ParameterValue
  /// CHECK:     <<Conv:b\d+>>    TypeConversion [<<Param>>]
  /// CHECK:     <<Return:v\d+>>  Return [<<Conv>>]

  /// CHECK-START: int Main.$noinline$testCharToCharToByte(char) instruction_simplifier (after)
  /// CHECK-NOT:                  Shl

  /// CHECK-START: int Main.$noinline$testCharToCharToByte(char) instruction_simplifier (after)
  /// CHECK-NOT:                  Shr
  private static int $noinline$testCharToCharToByte(char arg) {
    return (byte) ((arg << 16) >>> 16);
  }

  public static void main(String[] args) throws Exception {
    Class smaliTests2 = Class.forName("SmaliTests2");
    Method $noinline$XorAllOnes = smaliTests2.getMethod("$noinline$XorAllOnes", int.class);
    Method $noinline$NotNot1 = smaliTests2.getMethod("$noinline$NotNot1", long.class);
    Method $noinline$NotNot2 = smaliTests2.getMethod("$noinline$NotNot2", int.class);
    Method $noinline$NotNotBool = smaliTests2.getMethod("$noinline$NotNotBool", boolean.class);
    Method $noinline$bug68142795Short = smaliTests2.getMethod("$noinline$bug68142795Short", short.class);
    Method $noinline$bug68142795Boolean = smaliTests2.getMethod("$noinline$bug68142795Boolean", boolean.class);

    int arg = 123456;
    float floatArg = 123456.125f;

    assertLongEquals(arg, $noinline$Add0(arg));
    assertIntEquals(5, $noinline$AddAddSubAddConst(1));
    assertIntEquals(arg, $noinline$AndAllOnes(arg));
    assertLongEquals(arg, $noinline$Div1(arg));
    assertIntEquals(-arg, $noinline$DivN1(arg));
    assertLongEquals(arg, $noinline$Mul1(arg));
    assertIntEquals(-arg, $noinline$MulN1(arg));
    assertLongEquals((128 * arg), $noinline$MulPowerOfTwo128(arg));
    assertLongEquals(2640, $noinline$MulMulMulConst(2));
    assertIntEquals(arg, $noinline$Or0(arg));
    assertLongEquals(arg, $noinline$OrSame(arg));
    assertIntEquals(arg, $noinline$Shl0(arg));
    assertLongEquals(arg, $noinline$Shr0(arg));
    assertLongEquals(arg, $noinline$Shr64(arg));
    assertLongEquals(arg, $noinline$Sub0(arg));
    assertIntEquals(-arg, $noinline$SubAliasNeg(arg));
    assertIntEquals(9, $noinline$SubAddConst1(2));
    assertIntEquals(-2, $noinline$SubAddConst2(3));
    assertLongEquals(3, $noinline$SubSubConst(4));
    assertLongEquals(arg, $noinline$UShr0(arg));
    assertIntEquals(arg, $noinline$Xor0(arg));
    assertIntEquals(~arg, (int)$noinline$XorAllOnes.invoke(null, arg));
    assertIntEquals(-(arg + arg + 1), $noinline$AddNegs1(arg, arg + 1));
    assertIntEquals(-(arg + arg + 1), $noinline$AddNegs2(arg, arg + 1));
    assertLongEquals(-(2 * arg + 1), $noinline$AddNegs3(arg, arg + 1));
    assertLongEquals(1, $noinline$AddNeg1(arg, arg + 1));
    assertLongEquals(-1, $noinline$AddNeg2(arg, arg + 1));
    assertLongEquals(arg, $noinline$NegNeg1(arg));
    assertIntEquals(0, $noinline$NegNeg2(arg));
    assertLongEquals(arg, $noinline$NegNeg3(arg));
    assertIntEquals(1, $noinline$NegSub1(arg, arg + 1));
    assertIntEquals(1, $noinline$NegSub2(arg, arg + 1));
    assertLongEquals(arg, (long)$noinline$NotNot1.invoke(null, arg));
    assertLongEquals(arg, $noinline$runSmaliTest2Long("$noinline$NotNot1", arg));
    assertIntEquals(-1, (int)$noinline$NotNot2.invoke(null, arg));
    assertIntEquals(-1, $noinline$runSmaliTestInt("2", "$noinline$NotNot2", arg));
    assertIntEquals(-(arg + arg + 1), $noinline$SubNeg1(arg, arg + 1));
    assertIntEquals(-(arg + arg + 1), $noinline$SubNeg2(arg, arg + 1));
    assertLongEquals(-(2 * arg + 1), $noinline$SubNeg3(arg, arg + 1));
    assertBooleanEquals(true, $noinline$EqualBoolVsIntConst(true));
    assertBooleanEquals(true, $noinline$EqualBoolVsIntConst(true));
    assertBooleanEquals(false, $noinline$NotEqualBoolVsIntConst(false));
    assertBooleanEquals(false, $noinline$NotEqualBoolVsIntConst(false));
    assertBooleanEquals(true, (boolean)$noinline$NotNotBool.invoke(null, true));
    assertBooleanEquals(true, $noinline$runSmaliTest2Boolean("$noinline$NotNotBool", true));
    assertBooleanEquals(false, (boolean)$noinline$NotNotBool.invoke(null, false));
    assertBooleanEquals(false, $noinline$runSmaliTest2Boolean("$noinline$NotNotBool", false));
    assertFloatEquals(50.0f, $noinline$Div2(100.0f));
    assertDoubleEquals(75.0, $noinline$Div2(150.0));
    assertFloatEquals(-400.0f, $noinline$DivMP25(100.0f));
    assertDoubleEquals(-600.0, $noinline$DivMP25(150.0));
    assertIntEquals(0xc, $noinline$UShr28And15(0xc1234567));
    assertLongEquals(0xcL, $noinline$UShr60And15(0xc123456787654321L));
    assertIntEquals(0x4, $noinline$UShr28And7(0xc1234567));
    assertLongEquals(0x4L, $noinline$UShr60And7(0xc123456787654321L));
    assertIntEquals(0xc1, $noinline$Shr24And255(0xc1234567));
    assertIntEquals(0x60, $noinline$Shr25And127(0xc1234567));
    assertLongEquals(0xc1L, $noinline$Shr56And255(0xc123456787654321L));
    assertLongEquals(0x60L, $noinline$Shr57And127(0xc123456787654321L));
    assertIntEquals(0x41, $noinline$Shr24And127(0xc1234567));
    assertLongEquals(0x41L, $noinline$Shr56And127(0xc123456787654321L));
    assertIntEquals(0, $noinline$mulPow2Plus1(0));
    assertIntEquals(9, $noinline$mulPow2Plus1(1));
    assertIntEquals(18, $noinline$mulPow2Plus1(2));
    assertIntEquals(900, $noinline$mulPow2Plus1(100));
    assertIntEquals(111105, $noinline$mulPow2Plus1(12345));
    assertLongEquals(0, $noinline$mulPow2Minus1(0));
    assertLongEquals(31, $noinline$mulPow2Minus1(1));
    assertLongEquals(62, $noinline$mulPow2Minus1(2));
    assertLongEquals(3100, $noinline$mulPow2Minus1(100));
    assertLongEquals(382695, $noinline$mulPow2Minus1(12345));

    booleanField = false;
    assertIntEquals($noinline$booleanFieldNotEqualOne(), 54);
    assertIntEquals($noinline$booleanFieldEqualZero(), 54);
    booleanField = true;
    assertIntEquals(13, $noinline$booleanFieldNotEqualOne());
    assertIntEquals(13, $noinline$booleanFieldEqualZero());
    assertIntEquals(54, $noinline$intConditionNotEqualOne(6));
    assertIntEquals(13, $noinline$intConditionNotEqualOne(43));
    assertIntEquals(54, $noinline$intConditionEqualZero(6));
    assertIntEquals(13, $noinline$intConditionEqualZero(43));
    assertIntEquals(54, $noinline$floatConditionNotEqualOne(6.0f));
    assertIntEquals(13, $noinline$floatConditionNotEqualOne(43.0f));
    assertIntEquals(54, $noinline$doubleConditionEqualZero(6.0));
    assertIntEquals(13, $noinline$doubleConditionEqualZero(43.0));

    assertIntEquals(1234567, $noinline$intToDoubleToInt(1234567));
    assertIntEquals(Integer.MIN_VALUE, $noinline$intToDoubleToInt(Integer.MIN_VALUE));
    assertIntEquals(Integer.MAX_VALUE, $noinline$intToDoubleToInt(Integer.MAX_VALUE));
    assertStringEquals("d=7654321.0, i=7654321", $noinline$intToDoubleToIntPrint(7654321));
    assertIntEquals(12, $noinline$byteToDoubleToInt((byte) 12));
    assertIntEquals(Byte.MIN_VALUE, $noinline$byteToDoubleToInt(Byte.MIN_VALUE));
    assertIntEquals(Byte.MAX_VALUE, $noinline$byteToDoubleToInt(Byte.MAX_VALUE));
    assertIntEquals(11, $noinline$floatToDoubleToInt(11.3f));
    assertStringEquals("d=12.25, i=12", $noinline$floatToDoubleToIntPrint(12.25f));
    assertIntEquals(123, $noinline$byteToDoubleToShort((byte) 123));
    assertIntEquals(Byte.MIN_VALUE, $noinline$byteToDoubleToShort(Byte.MIN_VALUE));
    assertIntEquals(Byte.MAX_VALUE, $noinline$byteToDoubleToShort(Byte.MAX_VALUE));
    assertIntEquals(1234, $noinline$charToDoubleToShort((char) 1234));
    assertIntEquals(Character.MIN_VALUE, $noinline$charToDoubleToShort(Character.MIN_VALUE));
    assertIntEquals(/* sign-extended */ -1, $noinline$charToDoubleToShort(Character.MAX_VALUE));
    assertIntEquals(12345, $noinline$floatToIntToShort(12345.75f));
    assertIntEquals(Short.MAX_VALUE, $noinline$floatToIntToShort((float)(Short.MIN_VALUE - 1)));
    assertIntEquals(Short.MIN_VALUE, $noinline$floatToIntToShort((float)(Short.MAX_VALUE + 1)));
    assertIntEquals(-54321, $noinline$intToFloatToInt(-54321));
    assertDoubleEquals((double) 0x12345678, $noinline$longToIntToDouble(0x1234567812345678L));
    assertDoubleEquals(0.0, $noinline$longToIntToDouble(Long.MIN_VALUE));
    assertDoubleEquals(-1.0, $noinline$longToIntToDouble(Long.MAX_VALUE));
    assertLongEquals(0x0000000012345678L, $noinline$longToIntToLong(0x1234567812345678L));
    assertLongEquals(0xffffffff87654321L, $noinline$longToIntToLong(0x1234567887654321L));
    assertLongEquals(0L, $noinline$longToIntToLong(Long.MIN_VALUE));
    assertLongEquals(-1L, $noinline$longToIntToLong(Long.MAX_VALUE));
    assertIntEquals((short) -5678, $noinline$shortToCharToShort((short) -5678));
    assertIntEquals(Short.MIN_VALUE, $noinline$shortToCharToShort(Short.MIN_VALUE));
    assertIntEquals(Short.MAX_VALUE, $noinline$shortToCharToShort(Short.MAX_VALUE));
    assertIntEquals(5678, $noinline$shortToLongToInt((short) 5678));
    assertIntEquals(Short.MIN_VALUE, $noinline$shortToLongToInt(Short.MIN_VALUE));
    assertIntEquals(Short.MAX_VALUE, $noinline$shortToLongToInt(Short.MAX_VALUE));
    assertIntEquals(0x34, $noinline$shortToCharToByte((short) 0x1234));
    assertIntEquals(-0x10, $noinline$shortToCharToByte((short) 0x12f0));
    assertIntEquals(0, $noinline$shortToCharToByte(Short.MIN_VALUE));
    assertIntEquals(-1, $noinline$shortToCharToByte(Short.MAX_VALUE));
    assertStringEquals("c=1025, b=1", $noinline$shortToCharToBytePrint((short) 1025));
    assertStringEquals("c=1023, b=-1", $noinline$shortToCharToBytePrint((short) 1023));
    assertStringEquals("c=65535, b=-1", $noinline$shortToCharToBytePrint((short) -1));

    assertLongEquals(0x55411410L, $noinline$intAndSmallLongConstant(0x55555555));
    assertLongEquals(0xffffffffaa028aa2L, $noinline$intAndSmallLongConstant(0xaaaaaaaa));
    assertLongEquals(0x44101440L, $noinline$intAndLargeLongConstant(0x55555555));
    assertLongEquals(0x208a002aaL, $noinline$intAndLargeLongConstant(0xaaaaaaaa));
    assertLongEquals(7L, $noinline$intShr28And15L(0x76543210));

    assertIntEquals(0x21, $noinline$longAnd0xffToByte(0x1234432112344321L));
    assertIntEquals(0, $noinline$longAnd0xffToByte(Long.MIN_VALUE));
    assertIntEquals(-1, $noinline$longAnd0xffToByte(Long.MAX_VALUE));
    assertIntEquals(0x1234, $noinline$intAnd0x1ffffToChar(0x43211234));
    assertIntEquals(0, $noinline$intAnd0x1ffffToChar(Integer.MIN_VALUE));
    assertIntEquals(Character.MAX_VALUE, $noinline$intAnd0x1ffffToChar(Integer.MAX_VALUE));
    assertIntEquals(0x4321, $noinline$intAnd0x17fffToShort(0x87654321));
    assertIntEquals(0x0888, $noinline$intAnd0x17fffToShort(0x88888888));
    assertIntEquals(0, $noinline$intAnd0x17fffToShort(Integer.MIN_VALUE));
    assertIntEquals(Short.MAX_VALUE, $noinline$intAnd0x17fffToShort(Integer.MAX_VALUE));

    assertDoubleEquals(0.0, $noinline$shortAnd0xffffToShortToDouble((short) 0));
    assertDoubleEquals(1.0, $noinline$shortAnd0xffffToShortToDouble((short) 1));
    assertDoubleEquals(-2.0, $noinline$shortAnd0xffffToShortToDouble((short) -2));
    assertDoubleEquals(12345.0, $noinline$shortAnd0xffffToShortToDouble((short) 12345));
    assertDoubleEquals((double)Short.MAX_VALUE,
                       $noinline$shortAnd0xffffToShortToDouble(Short.MAX_VALUE));
    assertDoubleEquals((double)Short.MIN_VALUE,
                       $noinline$shortAnd0xffffToShortToDouble(Short.MIN_VALUE));

    assertIntEquals(13, $noinline$intReverseCondition(41));
    assertIntEquals(13, $noinline$intReverseConditionNaN(-5));

    for (String condition : new String[] { "Equal", "NotEqual" }) {
      for (String constant : new String[] { "True", "False" }) {
        for (String side : new String[] { "Rhs", "Lhs" }) {
          String name = condition + constant + side;
          assertIntEquals(5, $noinline$runSmaliTest(name, true));
          assertIntEquals(3, $noinline$runSmaliTest(name, false));
        }
      }
    }

    assertIntEquals(0, $noinline$runSmaliTestInt("AddSubConst", 1));
    assertIntEquals(3, $noinline$runSmaliTestInt("SubAddConst", 2));
    assertIntEquals(-16, $noinline$runSmaliTestInt("SubSubConst1", 3));
    assertIntEquals(-5, $noinline$runSmaliTestInt("SubSubConst2", 4));
    assertIntEquals(26, $noinline$runSmaliTestInt("SubSubConst3", 5));
    assertIntEquals(0x5e6f7808, $noinline$intUnnecessaryShiftMasking(0xabcdef01, 3));
    assertIntEquals(0x5e6f7808, $noinline$intUnnecessaryShiftMasking(0xabcdef01, 3 + 32));
    assertLongEquals(0xffffffffffffeaf3L,
                     $noinline$longUnnecessaryShiftMasking(0xabcdef0123456789L, 50));
    assertLongEquals(0xffffffffffffeaf3L,
                     $noinline$longUnnecessaryShiftMasking(0xabcdef0123456789L, 50 + 64));
    assertIntEquals(0x2af37b, $noinline$intUnnecessaryWiderShiftMasking(0xabcdef01, 10));
    assertIntEquals(0x2af37b, $noinline$intUnnecessaryWiderShiftMasking(0xabcdef01, 10 + 128));
    assertLongEquals(0xaf37bc048d159e24L,
                     $noinline$longSmallerShiftMasking(0xabcdef0123456789L, 2));
    assertLongEquals(0xaf37bc048d159e24L,
                     $noinline$longSmallerShiftMasking(0xabcdef0123456789L, 2 + 256));
    assertIntEquals(0xfffd5e7c, $noinline$otherUseOfUnnecessaryShiftMasking(0xabcdef01, 13));
    assertIntEquals(0xfffd5e7c, $noinline$otherUseOfUnnecessaryShiftMasking(0xabcdef01, 13 + 512));
    assertIntEquals(0x5f49eb48, $noinline$intUnnecessaryShiftModifications(0xabcdef01, 2));
    assertIntEquals(0xbd4c29b0, $noinline$intUnnecessaryShiftModifications(0xabcdef01, 3));
    assertIntEquals(0xc0fed1ca, $noinline$intNecessaryShiftModifications(0xabcdef01, 2));
    assertIntEquals(0x03578ebc, $noinline$intNecessaryShiftModifications(0xabcdef01, 3));

    assertIntEquals(654321, $noinline$intAddSubSimplifyArg1(arg, 654321));
    assertIntEquals(arg, $noinline$intAddSubSimplifyArg2(arg, 654321));
    assertIntEquals(arg, $noinline$intSubAddSimplifyLeft(arg, 654321));
    assertIntEquals(arg, $noinline$intSubAddSimplifyRight(arg, 654321));
    assertFloatEquals(654321.125f, $noinline$floatAddSubSimplifyArg1(floatArg, 654321.125f));
    assertFloatEquals(floatArg, $noinline$floatAddSubSimplifyArg2(floatArg, 654321.125f));
    assertFloatEquals(floatArg, $noinline$floatSubAddSimplifyLeft(floatArg, 654321.125f));
    assertFloatEquals(floatArg, $noinline$floatSubAddSimplifyRight(floatArg, 654321.125f));

    // Sub/Add and Sub/Sub simplifications
    final int[] int_inputs = {0, 1, -1, Integer.MIN_VALUE, Integer.MAX_VALUE, 42, -9000};
    for (int x : int_inputs) {
      for (int y : int_inputs) {
        // y - (x + y) = -x
        assertIntEquals(-x, $noinline$testSubAddInt(x, y));
        // x - (x + y) = -y.
        assertIntEquals(-y, $noinline$testSubAddOtherVersionInt(x, y));
        // (x - y) - x = -y.
        assertIntEquals(-y, $noinline$testSubSubInt(x, y));
        // x - (x - y) = y.
        assertIntEquals(y, $noinline$testSubSubOtherVersionInt(x, y));
      }
    }

    final long[] long_inputs = {0L, 1L, -1L, Long.MIN_VALUE, Long.MAX_VALUE, 0x100000000L,
            0x100000001L, -9000L, 0x0123456789ABCDEFL};
    for (long x : long_inputs) {
      for (long y : long_inputs) {
        // y - (x + y) = -x
        assertLongEquals(-x, $noinline$testSubAddLong(x, y));
        // x - (x + y) = -y.
        assertLongEquals(-y, $noinline$testSubAddOtherVersionLong(x, y));
        // (x - y) - x = -y.
        assertLongEquals(-y, $noinline$testSubSubLong(x, y));
        // x - (x - y) = y.
        assertLongEquals(y, $noinline$testSubSubOtherVersionLong(x, y));
      }
    }

    Main m = new Main();
    m.instanceByteField = -1;
    assertIntEquals(0xff, $noinline$getUint8FromInstanceByteField(m));
    staticByteField = -2;
    assertIntEquals(0xfe, $noinline$getUint8FromStaticByteField());
    assertIntEquals(0xfd, $noinline$getUint8FromByteArray(new byte[] { -3 }));
    m.instanceShortField = -4;
    assertIntEquals(0xfffc, $noinline$getUint16FromInstanceShortField(m));
    staticShortField = -5;
    assertIntEquals(0xfffb, $noinline$getUint16FromStaticShortField());
    assertIntEquals(0xfffa, $noinline$getUint16FromShortArray(new short[] { -6 }));
    m.instanceCharField = 0xfff9;
    assertIntEquals(-7, $noinline$getInt16FromInstanceCharField(m));
    staticCharField = 0xfff8;
    assertIntEquals(-8, $noinline$getInt16FromStaticCharField());
    assertIntEquals(-9, $noinline$getInt16FromCharArray(new char[] { 0xfff7 }));

    staticCharField = 0xfff6;
    assertIntEquals(0xf6, $noinline$getStaticCharFieldAnd0xff());

    staticByteField = -11;
    assertIntEquals(-11, $noinline$byteToUint8AndBack());

    m.instanceByteField = -12;
    assertIntEquals(0xfffff4f4, $noinline$getUint8FromInstanceByteFieldWithAnotherUse(m));

    assertIntEquals(0x21, $noinline$intAnd0xffToChar(0x87654321));
    assertIntEquals(0x121, $noinline$intAnd0x1ffToChar(0x87654321));

    m.instanceCharField = 'x';
    assertIntEquals('x', $noinline$getInstanceCharFieldAnd0x1ffff(m));

    assertIntEquals(0x7f, $noinline$bug68142795Byte((byte) 0x7f));
    assertIntEquals((byte) 0x80, $noinline$bug68142795Byte((byte) 0x80));
    assertIntEquals(0x7fff, (int)$noinline$bug68142795Short.invoke(null, (short) 0x7fff));
    assertIntEquals((short) 0x8000, (int)$noinline$bug68142795Short.invoke(null, (short) 0x8000));
    assertIntEquals(0, (int)$noinline$bug68142795Boolean.invoke(null, false));
    assertIntEquals(1, (int)$noinline$bug68142795Boolean.invoke(null, true));
    assertIntEquals(0x7f, $noinline$bug68142795Elaborate((byte) 0x7f));
    assertIntEquals((byte) 0x80, $noinline$bug68142795Elaborate((byte) 0x80));

    assertIntEquals(-1, $noinline$emptyStringIndexOf('a'));
    assertIntEquals(-1, $noinline$emptyStringIndexOf('Z'));
    assertIntEquals(-1, $noinline$emptyStringIndexOfAfter('a', 0));
    assertIntEquals(-1, $noinline$emptyStringIndexOfAfter('Z', -1));

    assertIntEquals(-1, $noinline$singleCharStringIndexOf('a'));
    assertIntEquals(0, $noinline$singleCharStringIndexOf('x'));
    assertIntEquals(-1, $noinline$singleCharStringIndexOf('Z'));
    assertIntEquals(-1, $noinline$singleCharStringIndexOfAfter('a', 0));
    assertIntEquals(0, $noinline$singleCharStringIndexOfAfter('x', -1));
    assertIntEquals(-1, $noinline$singleCharStringIndexOfAfter('x', 1));
    assertIntEquals(-1, $noinline$singleCharStringIndexOfAfter('Z', -1));

    assertIntEquals(0x65,$noinline$redundantAndNotRedundant(0x7fff6f45));
    assertIntEquals(0x5e,$noinline$redundantAndOtherUse(0x7fff6f45));

    assertIntEquals(0x79, $noinline$redundantAndShortToByteShift2((short) 0x6de7));
    assertIntEquals(0x79, $noinline$redundantAndShortToByteShift2((short) 0xfde7));
    assertIntEquals(0x7a, $noinline$redundantAndShortToByteShift5((short) 0x6f45));
    assertIntEquals(-6, $noinline$redundantAndShortToByteShift5((short) 0xff45));
    assertIntEquals(0x6f, $noinline$redundantAndShortToByteShift8((short) 0x6f45));
    assertIntEquals(-1, $noinline$redundantAndShortToByteShift8((short) 0xff45));
    assertIntEquals(0x37, $noinline$redundantAndShortToByteShift9NotRedundant((short) 0x6f45));
    assertIntEquals(127, $noinline$redundantAndShortToByteShift9NotRedundant((short) 0xff45));
    assertIntEquals(127, $noinline$redundantAndShortToByteShift10NotRedundant((short) 0xffff));
    assertIntEquals(6, $noinline$redundantAndShortToByteShift12((short) 0x6f45));
    assertIntEquals(-1, $noinline$redundantAndShortToByteShift12((short) 0xff45));
    assertIntEquals(0, $noinline$redundantAndShortToByteShift15((short) 0x6f45));
    assertIntEquals(-1, $noinline$redundantAndShortToByteShift15((short) 0xff45));
    assertIntEquals(0, $noinline$redundantAndShortToByteShift16((short) 0x6f45));
    assertIntEquals(-1, $noinline$redundantAndShortToByteShift16((short) 0xff45));
    assertIntEquals(0, $noinline$redundantAndShortToByteShortAndConstant((short) 0x6f45));
    assertIntEquals(-1, $noinline$redundantAndShortToByteShortAndConstant((short) 0xff45));

    assertIntEquals(0x79, $noinline$redundantAndIntToByteShift2(0x7fff6de7));
    assertIntEquals(0x79, $noinline$redundantAndIntToByteShift2(0xfffffde7));
    assertIntEquals(0x7a, $noinline$redundantAndIntToByteShift5(0x7fff6f45));
    assertIntEquals(-6, $noinline$redundantAndIntToByteShift5(0xffffff45));
    assertIntEquals(0x6f, $noinline$redundantAndIntToByteShift8(0x7fff6f45));
    assertIntEquals(-1, $noinline$redundantAndIntToByteShift8(0xffffff45));
    assertIntEquals(-73, $noinline$redundantAndIntToByteShift9(0x7fff6f45));
    assertIntEquals(-1, $noinline$redundantAndIntToByteShift9(0xffffff45));
    assertIntEquals(0x6f, $noinline$redundantAndIntToByteShift16(0x7f6fffff));
    assertIntEquals(0x6f, $noinline$redundantAndIntToByteShift16(0xff6fff45));
    assertIntEquals(0x6f, $noinline$redundantAndIntToByteShift20(0x76ffffff));
    assertIntEquals(0x6f, $noinline$redundantAndIntToByteShift20(0xf6ffff45));
    assertIntEquals(0x7f, $noinline$redundantAndIntToByteShift24(0x7fffffff));
    assertIntEquals(-1, $noinline$redundantAndIntToByteShift24(0xffffff45));
    assertIntEquals(0x3f, $noinline$redundantAndIntToByteShift25(0x7fffffff));
    assertIntEquals(-1, $noinline$redundantAndIntToByteShift25(0xffffff45));
    assertIntEquals(0, $noinline$redundantAndIntToByteShift31(0x7fffffff));
    assertIntEquals(-1, $noinline$redundantAndIntToByteShift31(0xffffff45));
    assertIntEquals(-1, $noinline$redundantAndIntToByteShortAndConstant(0x7fffff45));
    assertIntEquals(-1, $noinline$redundantAndIntToByteShortAndConstant(0xffffff45));
    assertIntEquals(111, $noinline$redundantAndRegressionNotConstant(-1, 0x6f45));

    assertIntEquals(50, $noinline$deadAddAfterUnrollingAndSimplification(new int[] { 0 }));

    for (int x : int_inputs) {
      for (int y : int_inputs) {
        assertIntEquals(x, $noinline$returnSecondIfEqualElseFirstInt(x, y));
        assertIntEquals(y, $noinline$returnSecondIfNotEqualElseFirstInt(x, y));
      }
    }

    for (long x : long_inputs) {
      for (long y : long_inputs) {
        assertLongEquals(x, $noinline$returnSecondIfEqualElseFirstLong(x, y));
        assertLongEquals(y, $noinline$returnSecondIfNotEqualElseFirstLong(x, y));
      }
    }

    assertIntEquals(0xaa, $noinline$testByteToIntAsUnsigned((byte) 0xaa));
    assertIntEquals(0xaabb, $noinline$testShortToIntAsUnsigned((short) 0xaabb));
    assertIntEquals(0xaabb, $noinline$testCharToIntAsUnsigned((char) 0xaabb));

    assertIntEquals(0xffffffaa, $noinline$testByteToIntAsSigned((byte) 0xaa));
    assertIntEquals(0x0a, $noinline$testByteToIntAsSigned((byte) 0x0a));
    assertIntEquals(0xffffaabb, $noinline$testShortToIntAsSigned((short) 0xaabb));
    assertIntEquals(0x0abb, $noinline$testShortToIntAsSigned((short) 0x0abb));
    assertIntEquals(0xffffaabb, $noinline$testCharToIntAsSigned((char) 0xaabb));
    assertIntEquals(0x0abb, $noinline$testCharToIntAsSigned((char) 0x0abb));

    assertIntEquals(0xbb, $noinline$testShortToByteToIntAsUnsigned((short) 0xaabb));
    assertIntEquals(0xbb, $noinline$testCharToByteToIntAsUnsigned((char) 0x0abb));

    assertIntEquals(0xffffffbb, $noinline$testShortToByteToIntAsSigned((short) 0xaabb));
    assertIntEquals(0x0b, $noinline$testShortToByteToIntAsSigned((short) 0xaa0b));
    assertIntEquals(0xffffffbb, $noinline$testCharToByteToIntAsSigned((char) 0x0abb));
    assertIntEquals(0x0b, $noinline$testCharToByteToIntAsSigned((char) 0x0a0b));

    assertIntEquals(0xdd, $noinline$testIntToUnsignedByteToInt(0xaabbccdd));
    assertIntEquals(0xccdd, $noinline$testIntToUnsignedShortToInt(0xaabbccdd));

    assertIntEquals(0xffffffdd, $noinline$testIntToSignedByteToInt(0xaabbccdd));
    assertIntEquals(0x0a, $noinline$testIntToSignedByteToInt(0x0a));
    assertIntEquals(0xffffccdd, $noinline$testIntToSignedShortToInt(0xaabbccdd));
    assertIntEquals(0x0abb, $noinline$testIntToSignedShortToInt(0x0abb));

    assertIntEquals(0xbb, $noinline$testCharToUnsignedByteToInt((char) 0xaabb));
    assertIntEquals(0xbb, $noinline$testShortToUnsignedByteToInt((short) 0xaabb));

    assertIntEquals(0xffffffbb, $noinline$testCharToSignedByteToInt((char) 0xaabb));
    assertIntEquals(0xffffffbb, $noinline$testShortToSignedByteToInt((short) 0xaabb));
    assertIntEquals(0x0b, $noinline$testCharToSignedByteToInt((char) 0xaa0b));
    assertIntEquals(0x0b, $noinline$testShortToSignedByteToInt((short) 0xaa0b));

    assertIntEquals(0xccdd,
        $noinline$testUnsignedPromotionWithHugeShiftAmount(0xaabbccdd));
    assertIntEquals(0xccdd,
        $noinline$testUnsignedPromotionWithHugeMismatchedShiftAmount(0xaabbccdd));

    assertLongEquals(0xdd, $noinline$testUnsignedPromotionToLong(0x11223344aabbccddL));
    assertLongEquals(0xffffffffffffffddL,
        $noinline$testSignedPromotionToLong(0x11223344aabbccddL));

    assertIntEquals(0xccdd,
        $noinline$testUnsignedPromotionWithNonConstantShiftAmount(0xaabbccdd, 16));
    assertIntEquals(0xffffffdd,
        $noinline$testSignedPromotionWithNonConstantShiftAmount(0xaabbccdd, 24));

    assertIntEquals(0xdd0000dd, $noinline$testUnsignedPromotionWithShlUse(0xaabbccdd));
    assertIntEquals(0xdcffffdd, $noinline$testSignedPromotionWithShlUse(0xaabbccdd));

    assertIntEquals(0x5d,
        $noinline$testUnsignedPromotionPatternWithIncorrectShiftAmountConstant(0xaabbccdd));
    assertIntEquals(0xffffffdd,
        $noinline$testSignedPromotionPatternWithIncorrectShiftAmountConstant(0xaabbccdd));

    assertIntEquals(0xdd00,
        $noinline$testUnsignedPromotionPatternWithDifferentShiftAmountConstants(0xaabbccdd));
    assertIntEquals(0xffffffee,
        $noinline$testSignedPromotionPatternWithDifferentShiftAmountConstants(0xaabbccdd));

    assertIntEquals(0xffaa, $noinline$testByteToByteToChar((byte) 0xaa));
    assertIntEquals(0x0a, $noinline$testByteToByteToChar((byte) 0x0a));

    assertIntEquals(0x0a, $noinline$testByteToShortToByte((byte) 0x0a));
    assertIntEquals(0xffffffaa, $noinline$testByteToShortToByte((byte) 0xaa));

    assertIntEquals(0x0a, $noinline$testUnsignedByteToUnsignedByteToByte((byte) 0x0a));
    assertIntEquals(0xffffffaa, $noinline$testUnsignedByteToUnsignedByteToByte((byte) 0xaa));

    assertIntEquals(0x0a, $noinline$testUnsignedByteToShortToByte((byte) 0x0a));
    assertIntEquals(0xffffffaa, $noinline$testUnsignedByteToShortToByte((byte) 0xaa));

    assertIntEquals(0x0a, $noinline$testUnsignedByteToCharToByte((byte) 0x0a));
    assertIntEquals(0xffffffaa, $noinline$testUnsignedByteToCharToByte((byte) 0xaa));

    assertIntEquals(0x0b, $noinline$testShortToShortToByte((short) 0xaa0b));
    assertIntEquals(0xffffffbb, $noinline$testShortToShortToByte((short) 0xaabb));

    assertIntEquals(0x0b, $noinline$testCharToCharToByte((char) 0xaa0b));
    assertIntEquals(0xffffffbb, $noinline$testCharToCharToByte((char) 0xaabb));
  }

  private static boolean $inline$true() { return true; }
  private static boolean $inline$false() { return false; }

  public static boolean booleanField;

  public static byte staticByteField;
  public static char staticCharField;
  public static short staticShortField;

  public byte instanceByteField;
  public char instanceCharField;
  public short instanceShortField;
}
