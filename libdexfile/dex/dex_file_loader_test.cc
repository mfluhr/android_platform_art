/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "dex_file.h"

#include <memory>

#include "base64_test_util.h"
#include "code_item_accessors-inl.h"
#include "descriptors_names.h"
#include "dex_file-inl.h"
#include "dex_file_loader.h"
#include "gtest/gtest.h"

namespace art {

class DexFileLoaderTest : public ::testing::Test {};

static constexpr char kLocationString[] = "/a/dex/file/location";

static inline std::vector<uint8_t> DecodeBase64Vec(const char* src) {
  std::vector<uint8_t> res;
  size_t size;
  std::unique_ptr<uint8_t[]> data(DecodeBase64(src, &size));
  res.resize(size);
  memcpy(res.data(), data.get(), size);
  return res;
}

// Although this is the same content logically as the Nested test dex,
// the DexFileHeader test is sensitive to subtle changes in the
// contents due to the checksum etc, so we embed the exact input here.
//
// class Nested {
//     class Inner {
//     }
// }
static const char kRawDex[] =
  "ZGV4CjAzNQAQedgAe7gM1B/WHsWJ6L7lGAISGC7yjD2IAwAAcAAAAHhWNBIAAAAAAAAAAMQCAAAP"
  "AAAAcAAAAAcAAACsAAAAAgAAAMgAAAABAAAA4AAAAAMAAADoAAAAAgAAAAABAABIAgAAQAEAAK4B"
  "AAC2AQAAvQEAAM0BAADXAQAA+wEAABsCAAA+AgAAUgIAAF8CAABiAgAAZgIAAHMCAAB5AgAAgQIA"
  "AAIAAAADAAAABAAAAAUAAAAGAAAABwAAAAkAAAAJAAAABgAAAAAAAAAKAAAABgAAAKgBAAAAAAEA"
  "DQAAAAAAAQAAAAAAAQAAAAAAAAAFAAAAAAAAAAAAAAAAAAAABQAAAAAAAAAIAAAAiAEAAKsCAAAA"
  "AAAAAQAAAAAAAAAFAAAAAAAAAAgAAACYAQAAuAIAAAAAAAACAAAAlAIAAJoCAAABAAAAowIAAAIA"
  "AgABAAAAiAIAAAYAAABbAQAAcBACAAAADgABAAEAAQAAAI4CAAAEAAAAcBACAAAADgBAAQAAAAAA"
  "AAAAAAAAAAAATAEAAAAAAAAAAAAAAAAAAAEAAAABAAY8aW5pdD4ABUlubmVyAA5MTmVzdGVkJElu"
  "bmVyOwAITE5lc3RlZDsAIkxkYWx2aWsvYW5ub3RhdGlvbi9FbmNsb3NpbmdDbGFzczsAHkxkYWx2"
  "aWsvYW5ub3RhdGlvbi9Jbm5lckNsYXNzOwAhTGRhbHZpay9hbm5vdGF0aW9uL01lbWJlckNsYXNz"
  "ZXM7ABJMamF2YS9sYW5nL09iamVjdDsAC05lc3RlZC5qYXZhAAFWAAJWTAALYWNjZXNzRmxhZ3MA"
  "BG5hbWUABnRoaXMkMAAFdmFsdWUAAgEABw4AAQAHDjwAAgIBDhgBAgMCCwQADBcBAgQBDhwBGAAA"
  "AQEAAJAgAICABNQCAAABAAGAgATwAgAAEAAAAAAAAAABAAAAAAAAAAEAAAAPAAAAcAAAAAIAAAAH"
  "AAAArAAAAAMAAAACAAAAyAAAAAQAAAABAAAA4AAAAAUAAAADAAAA6AAAAAYAAAACAAAAAAEAAAMQ"
  "AAACAAAAQAEAAAEgAAACAAAAVAEAAAYgAAACAAAAiAEAAAEQAAABAAAAqAEAAAIgAAAPAAAArgEA"
  "AAMgAAACAAAAiAIAAAQgAAADAAAAlAIAAAAgAAACAAAAqwIAAAAQAAABAAAAxAIAAA==";

// kRawDex{38,39,40,41} are dex'ed versions of the following Java source :
//
// public class Main {
//     public static void main(String[] foo) {
//     }
// }
//
// The dex file was manually edited to change its dex version code to 38
// or 39, respectively.
static const char kRawDex38[] =
  "ZGV4CjAzOAC4OovJlJ1089ikzK6asMf/f8qp3Kve5VsgAgAAcAAAAHhWNBIAAAAAAAAAAIwBAAAI"
  "AAAAcAAAAAQAAACQAAAAAgAAAKAAAAAAAAAAAAAAAAMAAAC4AAAAAQAAANAAAAAwAQAA8AAAACIB"
  "AAAqAQAAMgEAAEYBAABRAQAAVAEAAFgBAABtAQAAAQAAAAIAAAAEAAAABgAAAAQAAAACAAAAAAAA"
  "AAUAAAACAAAAHAEAAAAAAAAAAAAAAAABAAcAAAABAAAAAAAAAAAAAAABAAAAAQAAAAAAAAADAAAA"
  "AAAAAH4BAAAAAAAAAQABAAEAAABzAQAABAAAAHAQAgAAAA4AAQABAAAAAAB4AQAAAQAAAA4AAAAB"
  "AAAAAwAGPGluaXQ+AAZMTWFpbjsAEkxqYXZhL2xhbmcvT2JqZWN0OwAJTWFpbi5qYXZhAAFWAAJW"
  "TAATW0xqYXZhL2xhbmcvU3RyaW5nOwAEbWFpbgABAAcOAAMBAAcOAAAAAgAAgYAE8AEBCYgCDAAA"
  "AAAAAAABAAAAAAAAAAEAAAAIAAAAcAAAAAIAAAAEAAAAkAAAAAMAAAACAAAAoAAAAAUAAAADAAAA"
  "uAAAAAYAAAABAAAA0AAAAAEgAAACAAAA8AAAAAEQAAABAAAAHAEAAAIgAAAIAAAAIgEAAAMgAAAC"
  "AAAAcwEAAAAgAAABAAAAfgEAAAAQAAABAAAAjAEAAA==";

static const char kRawDex39[] =
  "ZGV4CjAzOQC4OovJlJ1089ikzK6asMf/f8qp3Kve5VsgAgAAcAAAAHhWNBIAAAAAAAAAAIwBAAAI"
  "AAAAcAAAAAQAAACQAAAAAgAAAKAAAAAAAAAAAAAAAAMAAAC4AAAAAQAAANAAAAAwAQAA8AAAACIB"
  "AAAqAQAAMgEAAEYBAABRAQAAVAEAAFgBAABtAQAAAQAAAAIAAAAEAAAABgAAAAQAAAACAAAAAAAA"
  "AAUAAAACAAAAHAEAAAAAAAAAAAAAAAABAAcAAAABAAAAAAAAAAAAAAABAAAAAQAAAAAAAAADAAAA"
  "AAAAAH4BAAAAAAAAAQABAAEAAABzAQAABAAAAHAQAgAAAA4AAQABAAAAAAB4AQAAAQAAAA4AAAAB"
  "AAAAAwAGPGluaXQ+AAZMTWFpbjsAEkxqYXZhL2xhbmcvT2JqZWN0OwAJTWFpbi5qYXZhAAFWAAJW"
  "TAATW0xqYXZhL2xhbmcvU3RyaW5nOwAEbWFpbgABAAcOAAMBAAcOAAAAAgAAgYAE8AEBCYgCDAAA"
  "AAAAAAABAAAAAAAAAAEAAAAIAAAAcAAAAAIAAAAEAAAAkAAAAAMAAAACAAAAoAAAAAUAAAADAAAA"
  "uAAAAAYAAAABAAAA0AAAAAEgAAACAAAA8AAAAAEQAAABAAAAHAEAAAIgAAAIAAAAIgEAAAMgAAAC"
  "AAAAcwEAAAAgAAABAAAAfgEAAAAQAAABAAAAjAEAAA==";

static const char kRawDex40[] =
  "ZGV4CjA0MAC4OovJlJ1089ikzK6asMf/f8qp3Kve5VsgAgAAcAAAAHhWNBIAAAAAAAAAAIwBAAAI"
  "AAAAcAAAAAQAAACQAAAAAgAAAKAAAAAAAAAAAAAAAAMAAAC4AAAAAQAAANAAAAAwAQAA8AAAACIB"
  "AAAqAQAAMgEAAEYBAABRAQAAVAEAAFgBAABtAQAAAQAAAAIAAAAEAAAABgAAAAQAAAACAAAAAAAA"
  "AAUAAAACAAAAHAEAAAAAAAAAAAAAAAABAAcAAAABAAAAAAAAAAAAAAABAAAAAQAAAAAAAAADAAAA"
  "AAAAAH4BAAAAAAAAAQABAAEAAABzAQAABAAAAHAQAgAAAA4AAQABAAAAAAB4AQAAAQAAAA4AAAAB"
  "AAAAAwAGPGluaXQ+AAZMTWFpbjsAEkxqYXZhL2xhbmcvT2JqZWN0OwAJTWFpbi5qYXZhAAFWAAJW"
  "TAATW0xqYXZhL2xhbmcvU3RyaW5nOwAEbWFpbgABAAcOAAMBAAcOAAAAAgAAgYAE8AEBCYgCDAAA"
  "AAAAAAABAAAAAAAAAAEAAAAIAAAAcAAAAAIAAAAEAAAAkAAAAAMAAAACAAAAoAAAAAUAAAADAAAA"
  "uAAAAAYAAAABAAAA0AAAAAEgAAACAAAA8AAAAAEQAAABAAAAHAEAAAIgAAAIAAAAIgEAAAMgAAAC"
  "AAAAcwEAAAAgAAABAAAAfgEAAAAQAAABAAAAjAEAAA==";

// Taken from 001-Main.
static const char kRawDex41[] =
  "ZGV4CjA0MQBBaEGw/8clTiOn3IafJ++m20gViy5Peh7UAgAAeAAAAHhWNBIAAAAAAAAAAEACAAAK"
  "AAAAeAAAAAQAAACgAAAAAgAAALAAAAAAAAAAAAAAAAMAAADIAAAAAQAAAOAAAAAAAAAAAAAAANQC"
  "AAAAAAAAOgEAAEIBAABKAQAAXgEAAGkBAABsAQAAcAEAAIUBAACLAQAAkQEAAAEAAAACAAAABAAA"
  "AAYAAAAEAAAAAgAAAAAAAAAFAAAAAgAAADQBAAAAAAAAAAAAAAAAAQAIAAAAAQAAAAAAAAAAAAAA"
  "AQAAAAEAAAAAAAAAAwAAAAAAAAAxAgAAAAAAAAEAAQABAAAAKgEAAAQAAABwEAIAAAAOAAEAAQAA"
  "AAAALgEAAAEAAAAOABEADgATAQgOAAABAAAAAwAGPGluaXQ+AAZMTWFpbjsAEkxqYXZhL2xhbmcv"
  "T2JqZWN0OwAJTWFpbi5qYXZhAAFWAAJWTAATW0xqYXZhL2xhbmcvU3RyaW5nOwAEYXJncwAEbWFp"
  "bgCdAX5+RDh7ImJhY2tlbmQiOiJkZXgiLCJjb21waWxhdGlvbi1tb2RlIjoiZGVidWciLCJoYXMt"
  "Y2hlY2tzdW1zIjpmYWxzZSwibWluLWFwaSI6MjYsInNoYS0xIjoiNTRjYmIzMTZlNGI3OWFhMDM1"
  "ZDUwMTM4ZTI3NjY4OGJiOTM5ZGIwNCIsInZlcnNpb24iOiI4LjMuMTQtZGV2In0AAAACAACBgASA"
  "AgEJmAIADAAAAAAAAAABAAAAAAAAAAEAAAAKAAAAeAAAAAIAAAAEAAAAoAAAAAMAAAACAAAAsAAA"
  "AAUAAAADAAAAyAAAAAYAAAABAAAA4AAAAAEgAAACAAAAAAEAAAMgAAACAAAAKgEAAAEQAAABAAAA"
  "NAEAAAIgAAAKAAAAOgEAAAAgAAABAAAAMQIAAAAQAAABAAAAQAIAAA==";

// Taken from 001-Main and modified.
static const char kRawDex42[] =
  "ZGV4CjA0MgBBaEGw/8clTiOn3IafJ++m20gViy5Peh7UAgAAeAAAAHhWNBIAAAAAAAAAAEACAAAK"
  "AAAAeAAAAAQAAACgAAAAAgAAALAAAAAAAAAAAAAAAAMAAADIAAAAAQAAAOAAAAAAAAAAAAAAANQC"
  "AAAAAAAAOgEAAEIBAABKAQAAXgEAAGkBAABsAQAAcAEAAIUBAACLAQAAkQEAAAEAAAACAAAABAAA"
  "AAYAAAAEAAAAAgAAAAAAAAAFAAAAAgAAADQBAAAAAAAAAAAAAAAAAQAIAAAAAQAAAAAAAAAAAAAA"
  "AQAAAAEAAAAAAAAAAwAAAAAAAAAxAgAAAAAAAAEAAQABAAAAKgEAAAQAAABwEAIAAAAOAAEAAQAA"
  "AAAALgEAAAEAAAAOABEADgATAQgOAAABAAAAAwAGPGluaXQ+AAZMTWFpbjsAEkxqYXZhL2xhbmcv"
  "T2JqZWN0OwAJTWFpbi5qYXZhAAFWAAJWTAATW0xqYXZhL2xhbmcvU3RyaW5nOwAEYXJncwAEbWFp"
  "bgCdAX5+RDh7ImJhY2tlbmQiOiJkZXgiLCJjb21waWxhdGlvbi1tb2RlIjoiZGVidWciLCJoYXMt"
  "Y2hlY2tzdW1zIjpmYWxzZSwibWluLWFwaSI6MjYsInNoYS0xIjoiNTRjYmIzMTZlNGI3OWFhMDM1"
  "ZDUwMTM4ZTI3NjY4OGJiOTM5ZGIwNCIsInZlcnNpb24iOiI4LjMuMTQtZGV2In0AAAACAACBgASA"
  "AgEJmAIADAAAAAAAAAABAAAAAAAAAAEAAAAKAAAAeAAAAAIAAAAEAAAAoAAAAAMAAAACAAAAsAAA"
  "AAUAAAADAAAAyAAAAAYAAAABAAAA4AAAAAEgAAACAAAAAAEAAAMgAAACAAAAKgEAAAEQAAABAAAA"
  "NAEAAAIgAAAKAAAAOgEAAAAgAAABAAAAMQIAAAAQAAABAAAAQAIAAA==";

// Taken from art-gtest-jars-MultiDex.jar
static const char kRawContainerDex[] =
  "ZGV4CjA0MQAAIUzJT/jrhaH3BHocZOpqBIO1QRkfBPE0AgAAeAAAAHhWNBIAAAAAAAAAAJQBAAAV"
  "AAAArAIAAAgAAAB4AAAABAAAAJgAAAABAAAAyAAAAAYAAADQAAAAAQAAAAABAAAAAAAAAAAAALwF"
  "AAAAAAAAAgAAAAMAAAAEAAAABQAAAAYAAAAHAAAACwAAAA0AAAABAAAABAAAAAAAAAALAAAABgAA"
  "AAAAAAAMAAAABgAAAHgBAAAMAAAABgAAAIABAAAFAAIAEQAAAAAAAQAAAAAAAAADABAAAAABAAEA"
  "AAAAAAEAAAAPAAAAAgACABIAAAADAAEAAAAAAAAAAAAAAAAAAwAAAAAAAAAIAAAAAAAAAIYBAAAA"
  "AAAAAQABAAEAAABmAQAABAAAAHAQBQAAAA4ABAABAAIAAABqAQAADwAAACIAAQBwEAIAAABiAQAA"
  "bhADAAAADAJuIAQAIQAOABEADgATAQ8OWgMAFAKWAAAAAAEAAAAEAAAAAQAAAAcAAAACAACAgASg"
  "AgEJuAINAAAAAAAAAAEAAAAAAAAAAgAAAAgAAAB4AAAAAwAAAAQAAACYAAAABAAAAAEAAADIAAAA"
  "BQAAAAYAAADQAAAABgAAAAEAAAAAAQAAASAAAAIAAAAgAQAAAyAAAAIAAABmAQAAARAAAAIAAAB4"
  "AQAAACAAAAEAAACGAQAAABAAAAEAAACUAQAAAQAAABUAAACsAgAAAiAAABUAAACYAwAAZGV4CjA0"
  "MQAxmn5fJHSijXMoNjKkUwU/LqsrYEld5QiIAwAAeAAAAHhWNBIAAAAAAAAAADQFAAAVAAAArAIA"
  "AAQAAAAAAwAAAgAAABADAAAAAAAAAAAAAAMAAAAoAwAAAQAAAEADAAAAAAAAAAAAALwFAAA0AgAA"
  "mAMAAKADAACjAwAAqwMAALUDAADMAwAA4AMAAPQDAAAIBAAAEwQAAB0EAAAqBAAALQQAADEEAABG"
  "BAAATAQAAFcEAABdBAAAYgQAAGsEAABzBAAAAwAAAAUAAAAGAAAACwAAAAEAAAACAAAAAAAAAAsA"
  "AAADAAAAAAAAAAAAAQAAAAAAAAAAAA8AAAABAAEAAAAAAAAAAAAAAAAAAQAAAAAAAAAKAAAAAAAA"
  "ACMFAAAAAAAAAgABAAAAAACQAwAAAwAAABoACQARAAAAAQABAAEAAACUAwAABAAAAHAQAgAAAA4A"
  "EwAOABEADgAGPGluaXQ+AAFMAAZMTWFpbjsACExTZWNvbmQ7ABVMamF2YS9pby9QcmludFN0cmVh"
  "bTsAEkxqYXZhL2xhbmcvT2JqZWN0OwASTGphdmEvbGFuZy9TdHJpbmc7ABJMamF2YS9sYW5nL1N5"
  "c3RlbTsACU1haW4uamF2YQAIT3JpZ2luYWwAC1NlY29uZC5qYXZhAAFWAAJWTAATW0xqYXZhL2xh"
  "bmcvU3RyaW5nOwAEYXJncwAJZ2V0U2Vjb25kAARtYWluAANvdXQAB3ByaW50bG4ABnNlY29uZACt"
  "AX5+RDh7ImJhY2tlbmQiOiJkZXgiLCJjb21waWxhdGlvbi1tb2RlIjoiZGVidWciLCJoYXMtY2hl"
  "Y2tzdW1zIjpmYWxzZSwibWluLWFwaSI6MTksInBsYXRmb3JtIjp0cnVlLCJzaGEtMSI6IjA0ZmZl"
  "ODMwMGM0MjlmMWFkMTZmM2E1Y2I0ZWQ2OTkyMTNlNGYyY2QiLCJ2ZXJzaW9uIjoiOC4yLjIxLWRl"
  "diJ9AAAAAQEAgIAE+AYBAeAGAAAACwAAAAAAAAABAAAANAIAAAEAAAAVAAAArAIAAAIAAAAEAAAA"
  "AAMAAAMAAAACAAAAEAMAAAUAAAADAAAAKAMAAAYAAAABAAAAQAMAAAEgAAACAAAAYAMAAAMgAAAC"
  "AAAAkAMAAAIgAAAVAAAAmAMAAAAgAAABAAAAIwUAAAAQAAABAAAANAUAAA==";

static const char kRawDexZeroLength[] =
  "UEsDBAoAAAAAAOhxAkkAAAAAAAAAAAAAAAALABwAY2xhc3Nlcy5kZXhVVAkAA2QNoVdnDaFXdXgL"
  "AAEE5AMBAASIEwAAUEsBAh4DCgAAAAAA6HECSQAAAAAAAAAAAAAAAAsAGAAAAAAAAAAAAKCBAAAA"
  "AGNsYXNzZXMuZGV4VVQFAANkDaFXdXgLAAEE5AMBAASIEwAAUEsFBgAAAAABAAEAUQAAAEUAAAAA"
  "AA==";

static const char kRawZipClassesDexPresent[] =
  "UEsDBBQAAAAIANVRN0ms99lIMQEAACACAAALABwAY2xhc3Nlcy5kZXhVVAkAAwFj5VcUY+VXdXgL"
  "AAEE5AMBAASIEwAAS0mt4DIwtmDYYdV9csrcks83lpxZN2vD8f/1p1beWX3vabQCEwNDAQMDQ0WY"
  "iRADFPQwMjBwMEDEWYB4AhADlTEsYEAAZiDeAcRApQwXgNgAyPgApJWAtBYQGwGxGxAHAnEIEEcA"
  "cS4jRD0T1Fw2KM0ENZMVypZhRLIIqIMdag9CBMFnhtJ1jDA5RrBcMSPE7AIBkIl8UFGgP6Fu4IOa"
  "wczAZpOZl1lix8Dm45uYmWfNIOSTlViWqJ+TmJeu75+UlZpcYs3ACZLSA4kzMIYxMIX5MAhHIykL"
  "LinKzEu3ZmDJBSoDOZiPgRlMgv3T2MDygZGRs4OJB8n9MBoWzrAwmQD1Eyy8WZHCmg0pvBkVIGpA"
  "Yc4oABEHhRuTAsRMUDwwQ9WAwoJBAaIGHE5Q9aB4BgBQSwECHgMUAAAACADVUTdJrPfZSDEBAAAg"
  "AgAACwAYAAAAAAAAAAAAoIEAAAAAY2xhc3Nlcy5kZXhVVAUAAwFj5Vd1eAsAAQTkAwEABIgTAABQ"
  "SwUGAAAAAAEAAQBRAAAAdgEAAAAA";

static const char kRawZipClassesDexAbsent[] =
  "UEsDBBQAAAAIANVRN0ms99lIMQEAACACAAAOABwAbm90Y2xhc3Nlcy5kZXhVVAkAAwFj5VcUY+VX"
  "dXgLAAEE5AMBAASIEwAAS0mt4DIwtmDYYdV9csrcks83lpxZN2vD8f/1p1beWX3vabQCEwNDAQMD"
  "Q0WYiRADFPQwMjBwMEDEWYB4AhADlTEsYEAAZiDeAcRApQwXgNgAyPgApJWAtBYQGwGxGxAHAnEI"
  "EEcAcS4jRD0T1Fw2KM0ENZMVypZhRLIIqIMdag9CBMFnhtJ1jDA5RrBcMSPE7AIBkIl8UFGgP6Fu"
  "4IOawczAZpOZl1lix8Dm45uYmWfNIOSTlViWqJ+TmJeu75+UlZpcYs3ACZLSA4kzMIYxMIX5MAhH"
  "IykLLinKzEu3ZmDJBSoDOZiPgRlMgv3T2MDygZGRs4OJB8n9MBoWzrAwmQD1Eyy8WZHCmg0pvBkV"
  "IGpAYc4oABEHhRuTAsRMUDwwQ9WAwoJBAaIGHE5Q9aB4BgBQSwECHgMUAAAACADVUTdJrPfZSDEB"
  "AAAgAgAADgAYAAAAAAAAAAAAoIEAAAAAbm90Y2xhc3Nlcy5kZXhVVAUAAwFj5Vd1eAsAAQTkAwEA"
  "BIgTAABQSwUGAAAAAAEAAQBUAAAAeQEAAAAA";

static const char kRawZipThreeDexFiles[] =
  "UEsDBBQAAAAIAP1WN0ms99lIMQEAACACAAAMABwAY2xhc3NlczIuZGV4VVQJAAOtbOVXrWzlV3V4"
  "CwABBOQDAQAEiBMAAEtJreAyMLZg2GHVfXLK3JLPN5acWTdrw/H/9adW3ll972m0AhMDQwEDA0NF"
  "mIkQAxT0MDIwcDBAxFmAeAIQA5UxLGBAAGYg3gHEQKUMF4DYAMj4AKSVgLQWEBsBsRsQBwJxCBBH"
  "AHEuI0Q9E9RcNijNBDWTFcqWYUSyCKiDHWoPQgTBZ4bSdYwwOUawXDEjxOwCAZCJfFBRoD+hbuCD"
  "msHMwGaTmZdZYsfA5uObmJlnzSDkk5VYlqifk5iXru+flJWaXGLNwAmS0gOJMzCGMTCF+TAIRyMp"
  "Cy4pysxLt2ZgyQUqAzmYj4EZTIL909jA8oGRkbODiQfJ/TAaFs6wMJkA9RMsvFmRwpoNKbwZFSBq"
  "QGHOKAARB4UbkwLETFA8MEPVgMKCQQGiBhxOUPWgeAYAUEsDBBQAAAAIAABXN0ms99lIMQEAACAC"
  "AAAMABwAY2xhc3NlczMuZGV4VVQJAAOvbOVXr2zlV3V4CwABBOQDAQAEiBMAAEtJreAyMLZg2GHV"
  "fXLK3JLPN5acWTdrw/H/9adW3ll972m0AhMDQwEDA0NFmIkQAxT0MDIwcDBAxFmAeAIQA5UxLGBA"
  "AGYg3gHEQKUMF4DYAMj4AKSVgLQWEBsBsRsQBwJxCBBHAHEuI0Q9E9RcNijNBDWTFcqWYUSyCKiD"
  "HWoPQgTBZ4bSdYwwOUawXDEjxOwCAZCJfFBRoD+hbuCDmsHMwGaTmZdZYsfA5uObmJlnzSDkk5VY"
  "lqifk5iXru+flJWaXGLNwAmS0gOJMzCGMTCF+TAIRyMpCy4pysxLt2ZgyQUqAzmYj4EZTIL909jA"
  "8oGRkbODiQfJ/TAaFs6wMJkA9RMsvFmRwpoNKbwZFSBqQGHOKAARB4UbkwLETFA8MEPVgMKCQQGi"
  "BhxOUPWgeAYAUEsDBBQAAAAIANVRN0ms99lIMQEAACACAAALABwAY2xhc3Nlcy5kZXhVVAkAAwFj"
  "5VetbOVXdXgLAAEE5AMBAASIEwAAS0mt4DIwtmDYYdV9csrcks83lpxZN2vD8f/1p1beWX3vabQC"
  "EwNDAQMDQ0WYiRADFPQwMjBwMEDEWYB4AhADlTEsYEAAZiDeAcRApQwXgNgAyPgApJWAtBYQGwGx"
  "GxAHAnEIEEcAcS4jRD0T1Fw2KM0ENZMVypZhRLIIqIMdag9CBMFnhtJ1jDA5RrBcMSPE7AIBkIl8"
  "UFGgP6Fu4IOawczAZpOZl1lix8Dm45uYmWfNIOSTlViWqJ+TmJeu75+UlZpcYs3ACZLSA4kzMIYx"
  "MIX5MAhHIykLLinKzEu3ZmDJBSoDOZiPgRlMgv3T2MDygZGRs4OJB8n9MBoWzrAwmQD1Eyy8WZHC"
  "mg0pvBkVIGpAYc4oABEHhRuTAsRMUDwwQ9WAwoJBAaIGHE5Q9aB4BgBQSwECHgMUAAAACAD9VjdJ"
  "rPfZSDEBAAAgAgAADAAYAAAAAAAAAAAAoIEAAAAAY2xhc3NlczIuZGV4VVQFAAOtbOVXdXgLAAEE"
  "5AMBAASIEwAAUEsBAh4DFAAAAAgAAFc3Saz32UgxAQAAIAIAAAwAGAAAAAAAAAAAAKCBdwEAAGNs"
  "YXNzZXMzLmRleFVUBQADr2zlV3V4CwABBOQDAQAEiBMAAFBLAQIeAxQAAAAIANVRN0ms99lIMQEA"
  "ACACAAALABgAAAAAAAAAAACgge4CAABjbGFzc2VzLmRleFVUBQADAWPlV3V4CwABBOQDAQAEiBMA"
  "AFBLBQYAAAAAAwADAPUAAABkBAAAAAA=";

static const char kRawDexBadMapOffset[] =
  "ZGV4CjAzNQAZKGSz85r+tXJ1I24FYi+FpQtWbXtelAmoAQAAcAAAAHhWNBIAAAAAAAAAAEAwIBAF"
  "AAAAcAAAAAMAAACEAAAAAQAAAJAAAAAAAAAAAAAAAAIAAACcAAAAAQAAAKwAAADcAAAAzAAAAOQA"
  "AADsAAAA9AAAAPkAAAANAQAAAgAAAAMAAAAEAAAABAAAAAIAAAAAAAAAAAAAAAAAAAABAAAAAAAA"
  "AAAAAAABAAAAAQAAAAAAAAABAAAAAAAAABUBAAAAAAAAAQABAAEAAAAQAQAABAAAAHAQAQAAAA4A"
  "Bjxpbml0PgAGQS5qYXZhAANMQTsAEkxqYXZhL2xhbmcvT2JqZWN0OwABVgABAAcOAAAAAQAAgYAE"
  "zAEACwAAAAAAAAABAAAAAAAAAAEAAAAFAAAAcAAAAAIAAAADAAAAhAAAAAMAAAABAAAAkAAAAAUA"
  "AAACAAAAnAAAAAYAAAABAAAArAAAAAEgAAABAAAAzAAAAAIgAAAFAAAA5AAAAAMgAAABAAAAEAEA"
  "AAAgAAABAAAAFQEAAAAQAAABAAAAIAEAAA==";

static const char kRawDexDebugInfoLocalNullType[] =
    "ZGV4CjAzNQA+Kwj2g6OZMH88OvK9Ey6ycdIsFCt18ED8AQAAcAAAAHhWNBIAAAAAAAAAAHQBAAAI"
    "AAAAcAAAAAQAAACQAAAAAgAAAKAAAAAAAAAAAAAAAAMAAAC4AAAAAQAAANAAAAAMAQAA8AAAABwB"
    "AAAkAQAALAEAAC8BAAA0AQAASAEAAEsBAABOAQAAAgAAAAMAAAAEAAAABQAAAAIAAAAAAAAAAAAA"
    "AAUAAAADAAAAAAAAAAEAAQAAAAAAAQAAAAYAAAACAAEAAAAAAAEAAAABAAAAAgAAAAAAAAABAAAA"
    "AAAAAGMBAAAAAAAAAQABAAEAAABUAQAABAAAAHAQAgAAAA4AAgABAAAAAABZAQAAAgAAABIQDwAG"
    "PGluaXQ+AAZBLmphdmEAAUkAA0xBOwASTGphdmEvbGFuZy9PYmplY3Q7AAFWAAFhAAR0aGlzAAEA"
    "Bw4AAwAHDh4DAAcAAAAAAQEAgYAE8AEBAIgCAAAACwAAAAAAAAABAAAAAAAAAAEAAAAIAAAAcAAA"
    "AAIAAAAEAAAAkAAAAAMAAAACAAAAoAAAAAUAAAADAAAAuAAAAAYAAAABAAAA0AAAAAEgAAACAAAA"
    "8AAAAAIgAAAIAAAAHAEAAAMgAAACAAAAVAEAAAAgAAABAAAAYwEAAAAQAAABAAAAdAEAAA==";

// Created from kRawDex38 by changing version to 35 and appending three entries
// to the map list, namely `kDexTypeMethodHandleItem`, `kDexTypeCallSiteIdItem`
// and `kDexTypeHiddenapiClassData`, each with size one and invalid offset 0xffff.
static const char kRawDexBadMapOffsets[] =
    "ZGV4CjAzNQC4OovJlJ1089ikzK6asMf/f8qp3Kve5VsgAgAAcAAAAHhWNBIAAAAAAAAAAIwBAAAI"
    "AAAAcAAAAAQAAACQAAAAAgAAAKAAAAAAAAAAAAAAAAMAAAC4AAAAAQAAANAAAAAwAQAA8AAAACIB"
    "AAAqAQAAMgEAAEYBAABRAQAAVAEAAFgBAABtAQAAAQAAAAIAAAAEAAAABgAAAAQAAAACAAAAAAAA"
    "AAUAAAACAAAAHAEAAAAAAAAAAAAAAAABAAcAAAABAAAAAAAAAAAAAAABAAAAAQAAAAAAAAADAAAA"
    "AAAAAH4BAAAAAAAAAQABAAEAAABzAQAABAAAAHAQAgAAAA4AAQABAAAAAAB4AQAAAQAAAA4AAAAB"
    "AAAAAwAGPGluaXQ+AAZMTWFpbjsAEkxqYXZhL2xhbmcvT2JqZWN0OwAJTWFpbi5qYXZhAAFWAAJW"
    "TAATW0xqYXZhL2xhbmcvU3RyaW5nOwAEbWFpbgABAAcOAAMBAAcOAAAAAgAAgYAE8AEBCYgCDwAA"
    "AAAAAAABAAAAAAAAAAEAAAAIAAAAcAAAAAIAAAAEAAAAkAAAAAMAAAACAAAAoAAAAAUAAAADAAAA"
    "uAAAAAYAAAABAAAA0AAAAAEgAAACAAAA8AAAAAEQAAABAAAAHAEAAAIgAAAIAAAAIgEAAAMgAAAC"
    "AAAAcwEAAAAgAAABAAAAfgEAAAAQAAABAAAAjAEAAAgAAAABAAAA//8AAAcAAAABAAAA//8AAADw"
    "AAABAAAA//8AAA==";

static const char kRawDexStringDataOOB[] =
    "ZGV4CjAzNQCeYAY06q0ySzKz8hklA3wUmxR8x10yt8X0AgAAcAAAAHhWNBIAAAAAAAAAAFQCAAAQAAAAcAAAAAcAAACw"
    "AAAAAwAAAMwAAAABAAAA8AAAAAQAAAD4AAAAAQAAABgBAAC8AQAAOAEAAH4BAACGAQABAAEAlQAAnQC0AQAAyAEAANwB"
    "AADwAQAA+"
    "wEAAP4BAAACAgAAFwIAAB0CAAAjAgAAKAIAADECAAACAAAAAwAAAAQAAAAFAAAABgAAAAgAAAAKAAAACAAAAAUAAAAAA"
    "AAACQAAAAUAAABwAQAACQAAAAUAAAB4AQAABAABAA0AAAAAAAAAAAAAAAAAAgAMAAAAAQABAA4AAAACAAAAAAAAAAAAA"
    "AABAAAAAgAAAAAAAAAHAAAAAAAAAEMCAAAAAAAAAQABAAEAAAA3AgAABAAAAHAQAwAAAA4AAwABAAIAAAA8AgAACAAAA"
    "GIAAAAaAQEAbiACABAADgABAAAAAwAAAAEAAAAGAAY8aW5pdD4ADUhlbGxvLCB3b3JsZCEABkxNYWluOwAVTGphdmEva"
    "W8vUHJpbnRTdHJlYW07ABJMamF2YS9sYW5nL09iamVjdDsAEkxqYXZhL2xhbmcvU3RyaW5nOwASTGphdmEvbGFuZy9Te"
    "XN0ZW07AAlNYWluLmphdmEAAVYAAlZMABNbTGphK2EvbGFuZy9TdHJpbmc7AARhcmdzAARtYWluAANvdXQAB3ByaW50b"
    "G4ABHRoaXMAEQAHDgATAQwHDngAAAACAACBgAS4AgEJ0AIAAAANAAAAAAAAAAEAAAAAAAAAAQAAABAAAABwAAAAAgAAA"
    "AcAAACwAAAAAwAAAAMAAADMAAAABAAAAAEAAADwAAAABQAAAAQAAAD4AAAABgAAAAEAAAAYAQAAASAAAAIAAAA4AQAAA"
    "RAAAAIAAABwAQAAAiAAABAAAAB+AQAAAyAAAAIAAAA3AgAAACAAAAEAAABDAgAAABAAAAEAAABUAgAA";

static const char kRawDexCodeItemOOB[] =
    "ZGV4CjAzNQBNRhvKLnmGPLR973zkwLwGomvp/qfZL080AgAAcAAAAHhWNBIAAAAA"
    "AAAAAKABAAAKAAAAcAAAAAQAAACYAAAAAgAAAKgAAAAAAAAAAAAAAAMAAADAAAAA"
    "AQAAANgAAAA8AQAACAAAACoBAAAxAQAA2gEAAE4BAABZAQAAXAEAAGABAAB1AQAA"
    "ewEAAIEBAAABAAAAAgAAAAQAAAAGAAAABAAAAAIAAAAAAAAABQAAAAIAAAAkAQAA"
    "AAAAAAAAAAAAAAEACAAAAAEAAAAAAAAAAAAAAAEAAAABAAAAAAAAAAMAAAAAAAAA"
    "kgEAAAAAAAABAAEAAQAAAIcBAAKSAAAAcBACAAAADgABAAEAAAAAAIwBAAABAAAA"
    "DgAAAAEAAAADAAY8aW5pdD4ABkxNYWluOwASTGphdmEvbGFuZy9PYmplY3Q7AAlN"
    "YWluLmphdmEAAVYAAlZMABNbTGphdmEvbGFuZy9TdHJpbmc7AARhcmdzAARtYWlu"
    "AAR0aGlzABEABw4A6QH4+PH////9+gAlgAT4AQEJkAIMAAAAAAAAAAEAAAAAAAAA"
    "AQAAAAoAAABwAAAAAgAAAAQAAACYAAAAAwAAAAIAAACoAAAABQAAAAMAAADAAAAA"
    "BgAAAAEAAADYAAAAASAAAAIAAAD4AAAAARAAAAEAAAAkAQAAAiAAAAoAAAAqAQAA"
    "AyAAAAIAAACHAQAAACAAAAEAAACSAQAAABAAAAEAAACgAQAA";

static const char kHiddenAPIClassDataBadOffset[] =
    "ZGV4CjAzNQA+Lt8iLnmGPLR973zkwLwGomvp/qfZL080AgAAcAAAAHhWNBIAAAAA"
    "AAAAAKABAAAKAAAAcAAAAAQAAACYAAAAAgAAAKgAAAAAAAAAAAAAAAMAAADAAAAA"
    "AQAAANgAAAA8AfoA+AAAACoBAAAyAQAAOgEAAE4BAABZAQAAXAEAAGABAAB1AQAA"
    "ewEAAIEBAAABAAAAAgAAAAQAAAAGAAAABAAAAAIAAAAAAAAABQAAAAIAAABHAQBP"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD/"
    "//////8DRwAAAAAAAAAAAAAAAAAIAAAAAAAAAPIAAAAIAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "ACAAAAACAAAABAAAAJgAAAADAAAAAgAAAKgAAAAFAAAAAwAAAMAAAAAGAAAAAQAA"
    "AAR0aGlzABEAAA4AEwEIBw4AAAACAAAlgPsH/gEJkAIMAAAAAAAAAAEAAAAAAAAA"
    "AQAAAAoAAABwAAAAAgAAAAQAAACYAAAAAwAAAAIAAACoAAAABQAAAAMAAADAAAAA"
    "BgAAAAEAAADYAAAABwAAAAIAAAD4AAAAAPAAAAAAAQAEAQAAAiAAAAoAAAAqAQAA"
    "AyAAAC0AAACHAQAAACAAAAEAAACSAQAAABAAAAEAAACgAQAA";

static const char kRawBadDebugInfoItem[] =
    "ZGV4CjAzOAAaShJb6q0xSzOzJXwUA/IZmxR8x10yt8X0AgAAcAAAAHhWNBIwQB8z"
    "AAAAAFQCAAAQAAAIcAAAAAcAAACwAAAAAwAAAMwAAAABAAAA8AAAAPz/9wD4AAAA"
    "AQAAOhgBAAC8AQA5AQAAAH4BAACAgAAAAAAAAAAAAAAEAAABAAn///kACAAAAAAA"
    "BgAAAAgACgIAABcCAAClAAIAIwIAACgCAAAxAgDeAgAAAEAAAAAEAAAABQBQAAYA"
    "AAAIAAAAAAAAAAAAAAcAAAEACf8Y+QAIAAAAAAAGAAAACAAAAAwAAICAgIAAAAIA"
    "IwL5ACgCAAAwAgACAQAAAAMAAAAEAAAABQBQAAYAAAAIAAAAAAAAAAADAAEACf//"
    "+QAIAAAAAAAGAAAACAAKAgAAFwMAAKUAAAMAAAAEAAAABQAAAAYAAAAEAAAAAgAA"
    "AAAAgIAACQAFEAAAAIAABACAgIAAAAAsCH4AAAAAAAYAAAAIAAAADAAAAAUAAAAC"
    "AAAAAICABICAgAAAAICAAAUAAAAGAAAACAAAAwAAAAAABAAAAAIAAAAM+f8ABQAA"
    "AAIAAACAgIAAAAQAgACAAIAALAB+AACAAAAGAAAACAAAAAwAAAAFAACAAgAAAACA"
    "gIAAAAAAAIAAAAAAAAAAAAAAAAQAAAACAAAAAACAAATmgICAAAAAABMAAAAAAACA"
    "AAAAAAgAAAAMAAAABQAAAAIAAAAAgIAEgICAAAAAgIAABQAAAAYAAAAIAAADAAAA"
    "AAAEAAAAAgAAAAwAAAAFAAAAAgANAAAAAAAjAAEAAAAAAAAABwAAABAAAABwAAAA"
    "AgAAAAcAAACwAAAAAyAAAEEAAADMAAAABAAAAAEnAADwAAAABQAAAAQAA/j4AAAA"
    "BgAAAFv///8bAQAAAQAAAAAAAG81AQAAARAAAAIAAABwAQAAAiAAABAAAAB+AQAA"
    "AwAAKQIAAAA3AgAAACAAAAEAAABDAgAAABAAAAEAAABUAgAA";

static const char kIncorrectSectionSizeInHeader[] =
    "ZGV4CjAzOACmfCim6q0xSzKzJXwUA/IZmxR8x10yt8X0AgAAcAAAAHhWNBIwQB8z"
    "AAAAAFQCAAABAT//cAAAAAcAAACwAAAAAwAAAMwAAAABAACA8AAAAPz/9wD4AAAA"
    "AQAAOhgBAAC8AzI0AQAAAH4BgAAAAQAAANgAAAApAQAAYQAAACoJWwAxAQAAOgAA"
    "zAAAAP////8QCQAABAAAADEBAAA6AADMAAAA/////xAJAAAEAAAAAAAAAIAAAAAA"
    "gAAAAAAAAAEAAADmAAAABAAAAAoAAACAAAAAAIAAAAAAgAAAAICAgAAAAAAAAAAA"
    "gIAAAAAAgAAAAICAgAAAAAAAAAAAgIAAAAAAAAAAAACAAAAAAHoAAACGgu//Nzk3"
    "QPUhAEEAAP//dgACAAAADQAAAOYAAAAAAAAAAAAATWFpbm7qYXZhEAD8//cA+AAA"
    "AAEAADoYAQAAvAMyNAEAAAB+AQAAgIAAAAAAAAAAAICAAAAAAIAAAACAgIAAAAAA"
    "AAAAAICAAAAAAAAAAAAAgAAAAAB6AAAAhoLv/zc5N0D1IQBBAAD//3YAAgAAAA0A"
    "AADmAAAAAAAAAAAAAE1haW5u6mF2YRAA/P/3APgAAADYAAAAKQEAAGEAAAAqCVsA"
    "MQEAADoAAMwAAAD/////EAkAAAQAAAAxAQAAOgAAzAAAAP////8QCQAATWFpbm7q"
    "YQB2YQFWAAJWGAATizUBAQmQBgwAAAAAAAAAAACHAQAAACAAAAEA+P9t+gAAABAA"
    "AAEABAAAAICAgACAAAAAAIAABQANAAAAAAAjAAEAAAAAAAAABwAABxAAAABwAAAA"
    "AgAAAAcAAACwAAAAACAABEEAAADMAAAABAAAAAEnAADwAAAABQAAAwAAAPj4AAAA"
    "BgAwqDYA//8YAQAAAQAAAAAAAG8zAQAAARAAAAIAAABwAQAAAiAAABAAAAB+AQAA"
    "AwAAKQIAAAAzAgAABiAAAAEAAABDAgAAABAAAAEAAABUAgAA";

static const char kFileSizeTooSmallInHeader[] =
    "ZGV4CjAzOADm+mgA5vpofOqtMUsBCAAAJAEAAAJ3AAABAAAAcQAA/////////0ES"
    "+//4mrr////u/wAAAAAAADv//0X/ZAEAAFwBAABgY2Q6JAEAAHsBAACBAQAAAQAA"
    "AAIAAAAFAAAEAAAAAAAAAA==";

static void DecodeDexFile(const char* base64, std::vector<uint8_t>* dex_bytes) {
  // decode base64
  CHECK(base64 != nullptr);
  *dex_bytes = DecodeBase64Vec(base64);
  CHECK_NE(dex_bytes->size(), 0u);
}

static bool OpenDexFilesBase64(const char* base64,
                               const char* location,
                               std::vector<uint8_t>* dex_bytes,
                               std::vector<std::unique_ptr<const DexFile>>* dex_files,
                               DexFileLoaderErrorCode* error_code,
                               std::string* error_msg) {
  DecodeDexFile(base64, dex_bytes);

  // read dex file(s)
  static constexpr bool kVerifyChecksum = true;
  std::vector<std::unique_ptr<const DexFile>> tmp;
  DexFileLoader dex_file_loader(dex_bytes->data(), dex_bytes->size(), location);
  bool success =
      dex_file_loader.Open(/* verify= */ true, kVerifyChecksum, error_code, error_msg, dex_files);
  return success;
}

static std::unique_ptr<const DexFile> OpenDexFileBase64(const char* base64,
                                                        const char* location,
                                                        std::vector<uint8_t>* dex_bytes,
                                                        size_t expected_dex_files = 1) {
  // read dex files.
  DexFileLoaderErrorCode error_code;
  std::string error_msg;
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  bool success = OpenDexFilesBase64(base64, location, dex_bytes, &dex_files, &error_code,
                                    &error_msg);
  CHECK(success) << error_msg;
  EXPECT_EQ(expected_dex_files, dex_files.size());
  return std::move(dex_files[0]);
}

static std::unique_ptr<const DexFile> OpenDexFileInMemoryBase64(const char* base64,
                                                                const char* location,
                                                                uint32_t location_checksum,
                                                                bool expect_success,
                                                                std::vector<uint8_t>* dex_bytes) {
  DecodeDexFile(base64, dex_bytes);

  std::string error_message;
  DexFileLoader dex_file_loader(dex_bytes->data(), dex_bytes->size(), location);
  std::unique_ptr<const DexFile> dex_file(dex_file_loader.Open(location_checksum,
                                                               /* oat_dex_file= */ nullptr,
                                                               /* verify= */ true,
                                                               /* verify_checksum= */ true,
                                                               &error_message));
  if (expect_success) {
    CHECK(dex_file != nullptr) << error_message;
  } else {
    CHECK(dex_file == nullptr) << "Expected dex file open to fail.";
  }
  return dex_file;
}

static void ValidateDexFileHeader(std::unique_ptr<const DexFile> dex_file) {
  static const DexFile::Magic kExpectedDexFileMagic = {
      0x64, 0x65, 0x78, 0x0a, 0x30, 0x33, 0x35, 0x00,  // "dex\n035\0".
  };
  static const DexFile::Sha1 kExpectedSha1 = {
      0x7b, 0xb8, 0x0c, 0xd4, 0x1f, 0xd6, 0x1e, 0xc5, 0x89, 0xe8,
      0xbe, 0xe5, 0x18, 0x02, 0x12, 0x18, 0x2e, 0xf2, 0x8c, 0x3d,
  };

  const DexFile::Header& header = dex_file->GetHeader();
  EXPECT_EQ(kExpectedDexFileMagic, header.magic_);
  EXPECT_EQ(0x00d87910U, header.checksum_);
  EXPECT_EQ(kExpectedSha1, header.signature_);
  EXPECT_EQ(904U, header.file_size_);
  EXPECT_EQ(112U, header.header_size_);
  EXPECT_EQ(0U, header.link_size_);
  EXPECT_EQ(0U, header.link_off_);
  EXPECT_EQ(15U, header.string_ids_size_);
  EXPECT_EQ(112U, header.string_ids_off_);
  EXPECT_EQ(7U, header.type_ids_size_);
  EXPECT_EQ(172U, header.type_ids_off_);
  EXPECT_EQ(2U, header.proto_ids_size_);
  EXPECT_EQ(200U, header.proto_ids_off_);
  EXPECT_EQ(1U, header.field_ids_size_);
  EXPECT_EQ(224U, header.field_ids_off_);
  EXPECT_EQ(3U, header.method_ids_size_);
  EXPECT_EQ(232U, header.method_ids_off_);
  EXPECT_EQ(2U, header.class_defs_size_);
  EXPECT_EQ(256U, header.class_defs_off_);
  EXPECT_EQ(584U, header.data_size_);
  EXPECT_EQ(320U, header.data_off_);

  EXPECT_EQ(header.checksum_, dex_file->GetLocationChecksum());
}

TEST_F(DexFileLoaderTest, Header) {
  std::vector<uint8_t> dex_bytes;
  std::unique_ptr<const DexFile> raw(OpenDexFileBase64(kRawDex, kLocationString, &dex_bytes));
  ValidateDexFileHeader(std::move(raw));
}

TEST_F(DexFileLoaderTest, HeaderInMemory) {
  std::vector<uint8_t> dex_bytes;
  std::unique_ptr<const DexFile> raw =
      OpenDexFileInMemoryBase64(kRawDex, kLocationString, 0x00d87910U, true, &dex_bytes);
  ValidateDexFileHeader(std::move(raw));
}

TEST_F(DexFileLoaderTest, Version38Accepted) {
  std::vector<uint8_t> dex_bytes;
  std::unique_ptr<const DexFile> raw(OpenDexFileBase64(kRawDex38, kLocationString, &dex_bytes));
  ASSERT_TRUE(raw.get() != nullptr);

  const DexFile::Header& header = raw->GetHeader();
  EXPECT_EQ(38u, header.GetVersion());
}

TEST_F(DexFileLoaderTest, Version39Accepted) {
  std::vector<uint8_t> dex_bytes;
  std::unique_ptr<const DexFile> raw(OpenDexFileBase64(kRawDex39, kLocationString, &dex_bytes));
  ASSERT_TRUE(raw.get() != nullptr);

  const DexFile::Header& header = raw->GetHeader();
  EXPECT_EQ(39u, header.GetVersion());
}

TEST_F(DexFileLoaderTest, Version40Accepted) {
  std::vector<uint8_t> dex_bytes;
  std::unique_ptr<const DexFile> raw(OpenDexFileBase64(kRawDex40, kLocationString, &dex_bytes));
  ASSERT_TRUE(raw.get() != nullptr);

  const DexFile::Header& header = raw->GetHeader();
  EXPECT_EQ(40u, header.GetVersion());
}

TEST_F(DexFileLoaderTest, Version41Accepted) {
  std::vector<uint8_t> dex_bytes;
  std::unique_ptr<const DexFile> raw(OpenDexFileBase64(kRawDex41, kLocationString, &dex_bytes, 1));
  ASSERT_TRUE(raw.get() != nullptr);

  const DexFile::Header& header = raw->GetHeader();
  EXPECT_EQ(41u, header.GetVersion());
}

TEST_F(DexFileLoaderTest, Version42Rejected) {
  std::vector<uint8_t> dex_bytes;
  DecodeDexFile(kRawDex42, &dex_bytes);

  static constexpr bool kVerifyChecksum = true;
  DexFileLoaderErrorCode error_code;
  std::string error_msg;
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  DexFileLoader dex_file_loader(dex_bytes.data(), dex_bytes.size(), kLocationString);
  ASSERT_FALSE(dex_file_loader.Open(
      /* verify= */ true, kVerifyChecksum, &error_code, &error_msg, &dex_files));
}

TEST_F(DexFileLoaderTest, ContainerDex) {
  std::vector<uint8_t> dex_bytes;
  std::unique_ptr<const DexFile> raw(
      OpenDexFileBase64(kRawContainerDex, kLocationString, &dex_bytes, 2));
  ASSERT_TRUE(raw.get() != nullptr);

  const DexFile::Header& header = raw->GetHeader();
  EXPECT_EQ(41u, header.GetVersion());
}

TEST_F(DexFileLoaderTest, ZeroLengthDexRejected) {
  std::vector<uint8_t> dex_bytes;
  DecodeDexFile(kRawDexZeroLength, &dex_bytes);

  static constexpr bool kVerifyChecksum = true;
  DexFileLoaderErrorCode error_code;
  std::string error_msg;
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  DexFileLoader dex_file_loader(dex_bytes.data(), dex_bytes.size(), kLocationString);
  ASSERT_FALSE(dex_file_loader.Open(
      /* verify= */ true, kVerifyChecksum, &error_code, &error_msg, &dex_files));
}

TEST_F(DexFileLoaderTest, GetMultiDexClassesDexName) {
  ASSERT_EQ("classes.dex", DexFileLoader::GetMultiDexClassesDexName(0));
  ASSERT_EQ("classes2.dex", DexFileLoader::GetMultiDexClassesDexName(1));
  ASSERT_EQ("classes3.dex", DexFileLoader::GetMultiDexClassesDexName(2));
  ASSERT_EQ("classes100.dex", DexFileLoader::GetMultiDexClassesDexName(99));
}

TEST_F(DexFileLoaderTest, GetMultiDexLocation) {
  std::string dex_location_str = "/system/app/framework.jar";
  const char* dex_location = dex_location_str.c_str();
  ASSERT_EQ("/system/app/framework.jar", DexFileLoader::GetMultiDexLocation(0, dex_location));
  ASSERT_EQ("/system/app/framework.jar!classes2.dex",
            DexFileLoader::GetMultiDexLocation(1, dex_location));
  ASSERT_EQ("/system/app/framework.jar!classes101.dex",
            DexFileLoader::GetMultiDexLocation(100, dex_location));
}

TEST(DexFileUtilsTest, GetBaseLocationAndMultiDexSuffix) {
  EXPECT_EQ("/foo/bar/baz.jar", DexFileLoader::GetBaseLocation("/foo/bar/baz.jar"));
  EXPECT_EQ("/foo/bar/baz.jar", DexFileLoader::GetBaseLocation("/foo/bar/baz.jar!classes2.dex"));
  EXPECT_EQ("/foo/bar/baz.jar", DexFileLoader::GetBaseLocation("/foo/bar/baz.jar!classes8.dex"));
  EXPECT_EQ("", DexFileLoader::GetMultiDexSuffix("/foo/bar/baz.jar"));
  EXPECT_EQ("!classes2.dex", DexFileLoader::GetMultiDexSuffix("/foo/bar/baz.jar!classes2.dex"));
  EXPECT_EQ("!classes8.dex", DexFileLoader::GetMultiDexSuffix("/foo/bar/baz.jar!classes8.dex"));
}

TEST_F(DexFileLoaderTest, ZipOpenClassesPresent) {
  std::vector<uint8_t> dex_bytes;
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  DexFileLoaderErrorCode error_code;
  std::string error_msg;
  ASSERT_TRUE(OpenDexFilesBase64(kRawZipClassesDexPresent,
                                 kLocationString,
                                 &dex_bytes,
                                 &dex_files,
                                 &error_code,
                                 &error_msg));
  EXPECT_EQ(dex_files.size(), 1u);
}

TEST_F(DexFileLoaderTest, ZipOpenClassesAbsent) {
  std::vector<uint8_t> dex_bytes;
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  DexFileLoaderErrorCode error_code;
  std::string error_msg;
  ASSERT_FALSE(OpenDexFilesBase64(kRawZipClassesDexAbsent,
                                  kLocationString,
                                  &dex_bytes,
                                  &dex_files,
                                  &error_code,
                                  &error_msg));
  EXPECT_EQ(error_code, DexFileLoaderErrorCode::kEntryNotFound);
  EXPECT_EQ(dex_files.size(), 0u);
}

TEST_F(DexFileLoaderTest, ZipOpenThreeDexFiles) {
  std::vector<uint8_t> dex_bytes;
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  DexFileLoaderErrorCode error_code;
  std::string error_msg;
  ASSERT_TRUE(OpenDexFilesBase64(kRawZipThreeDexFiles,
                                 kLocationString,
                                 &dex_bytes,
                                 &dex_files,
                                 &error_code,
                                 &error_msg));
  EXPECT_EQ(dex_files.size(), 3u);
}

TEST_F(DexFileLoaderTest, OpenDexBadMapOffset) {
  std::vector<uint8_t> dex_bytes;
  std::unique_ptr<const DexFile> raw =
      OpenDexFileInMemoryBase64(kRawDexBadMapOffset,
                                kLocationString,
                                0xb3642819U,
                                false,
                                &dex_bytes);
  EXPECT_EQ(raw, nullptr);
}

TEST_F(DexFileLoaderTest, OpenDexDebugInfoLocalNullType) {
  std::vector<uint8_t> dex_bytes;
  std::unique_ptr<const DexFile> raw = OpenDexFileInMemoryBase64(kRawDexDebugInfoLocalNullType,
                                                                 kLocationString,
                                                                 0xf25f2b38U,
                                                                 true,
                                                                 &dex_bytes);
  const dex::ClassDef& class_def = raw->GetClassDef(0);
  constexpr uint32_t kMethodIdx = 1;
  const dex::CodeItem* code_item = raw->GetCodeItem(raw->FindCodeItemOffset(class_def, kMethodIdx));
  CodeItemDebugInfoAccessor accessor(*raw, code_item, kMethodIdx);
  ASSERT_TRUE(accessor.DecodeDebugLocalInfo(true, 1, VoidFunctor()));
}

// Helper for tests that open and verify raw dex files to avoid boilerplate.
void OpenAndVerify(const char* dex_file_base64, bool expected_success) {
  std::vector<uint8_t> dex_bytes;
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  DexFileLoaderErrorCode error_code;
  std::string error_msg;
  bool success = OpenDexFilesBase64(
      dex_file_base64, kLocationString, &dex_bytes, &dex_files, &error_code, &error_msg);
  ASSERT_EQ(success, expected_success);
}

// Bad offset for `kDexTypeHiddenapiClassData` previously triggered a `DCHECK()`
// before verifying the dex file. We want to reject dex files with bad offsets
// without crashing, even in debug builds. b/281960267
TEST_F(DexFileLoaderTest, BadMapOffsets) {
  OpenAndVerify(kRawDexBadMapOffsets, /*expected_success=*/false);
}

// Generated dex file with a string data offset out of bounds. It should fail verification without
// crashing. b/280066537
TEST_F(DexFileLoaderTest, StringDataOffsetOutOfBounds) {
  OpenAndVerify(kRawDexStringDataOOB, /*expected_success=*/false);
}

TEST_F(DexFileLoaderTest, CodeItemOutOfBounds) {
  OpenAndVerify(kRawDexCodeItemOOB, /*expected_success=*/false);
}

TEST_F(DexFileLoaderTest, HiddenAPIClassDataBadOffset) {
  OpenAndVerify(kHiddenAPIClassDataBadOffset, /*expected_success=*/false);
}

TEST_F(DexFileLoaderTest, BadDebugInfoItem) {
  OpenAndVerify(kRawBadDebugInfoItem, /*expected_success=*/false);
}

TEST_F(DexFileLoaderTest, IncorrectSectionSizeInHeader) {
  OpenAndVerify(kIncorrectSectionSizeInHeader, /*expected_success=*/false);
}

TEST_F(DexFileLoaderTest, FileSizeTooSmallInHeader) {
  OpenAndVerify(kFileSizeTooSmallInHeader, /*expected_success=*/false);
}

}  // namespace art
