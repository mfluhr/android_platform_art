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
 *
 * Implementation file of the dexdump utility.
 *
 * This is a re-implementation of the original dexdump utility that was
 * based on Dalvik functions in libdex into a new dexdump that is now
 * based on Art functions in libart instead. The output is very similar to
 * to the original for correct DEX files. Error messages may differ, however.
 * Also, ODEX files are no longer supported.
 *
 * The dexdump tool is intended to mimic objdump.  When possible, use
 * similar command-line arguments.
 *
 * Differences between XML output and the "current.xml" file:
 * - classes in same package are not all grouped together; nothing is sorted
 * - no "deprecated" on fields and methods
 * - no parameter names
 * - no generic signatures on parameters, e.g. type="java.lang.Class&lt;?&gt;"
 * - class shows declared fields and methods; does not show inherited fields
 */

#include "dexdump.h"

#include <inttypes.h>
#include <stdio.h>

#include <cctype>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string_view>
#include <vector>

#include "android-base/file.h"
#include "android-base/logging.h"
#include "android-base/stringprintf.h"
#include "base/bit_utils.h"
#include "dex/class_accessor-inl.h"
#include "dex/code_item_accessors-inl.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_exception_helpers.h"
#include "dex/dex_file_loader.h"
#include "dex/dex_file_structs.h"
#include "dex/dex_file_types.h"
#include "dex/dex_instruction-inl.h"
#include "dexdump_cfg.h"

namespace art {

/*
 * Options parsed in main driver.
 */
struct Options gOptions;

/*
 * Output file. Defaults to stdout.
 */
FILE* gOutFile = stdout;

/*
 * Data types that match the definitions in the VM specification.
 */
using u1 = uint8_t;
using u2 = uint16_t;
using u4 = uint32_t;
using u8 = uint64_t;
using s1 = int8_t;
using s2 = int16_t;
using s4 = int32_t;
using s8 = int64_t;

/*
 * Basic information about a field or a method.
 */
struct FieldMethodInfo {
  const char* classDescriptor;
  const char* name;
  const char* signature;
};

/*
 * Flags for use with createAccessFlagStr().
 */
enum class AccessFor {
  kClass = 0,
  kMethod = 1,
  kField = 2,
  kCount
};
static constexpr int kNumFlags = 18;

/*
 * Gets 2 little-endian bytes.
 */
static inline u2 get2LE(unsigned char const* pSrc) {
  return pSrc[0] | (pSrc[1] << 8);
}

/*
 * Converts a single-character primitive type into human-readable form.
 */
static const char* primitiveTypeLabel(char typeChar) {
  switch (typeChar) {
    case 'B': return "byte";
    case 'C': return "char";
    case 'D': return "double";
    case 'F': return "float";
    case 'I': return "int";
    case 'J': return "long";
    case 'S': return "short";
    case 'V': return "void";
    case 'Z': return "boolean";
    default:  return "UNKNOWN";
  }  // switch
}

/*
 * Converts a type descriptor to human-readable "dotted" form.  For
 * example, "Ljava/lang/String;" becomes "java.lang.String", and
 * "[I" becomes "int[]".
 */
static std::unique_ptr<char[]> descriptorToDot(const char* str) {
  int targetLen = strlen(str);
  int offset = 0;

  // Strip leading [s; will be added to end.
  while (targetLen > 1 && str[offset] == '[') {
    offset++;
    targetLen--;
  }  // while

  const int arrayDepth = offset;

  if (targetLen == 1) {
    // Primitive type.
    str = primitiveTypeLabel(str[offset]);
    offset = 0;
    targetLen = strlen(str);
  } else {
    // Account for leading 'L' and trailing ';'.
    if (targetLen >= 2 && str[offset] == 'L' &&
        str[offset + targetLen - 1] == ';') {
      targetLen -= 2;
      offset++;
    }
  }

  // Copy class name over.
  std::unique_ptr<char[]> newStr(new char[targetLen + arrayDepth * 2 + 1]);
  int i = 0;
  for (; i < targetLen; i++) {
    const char ch = str[offset + i];
    newStr[i] = (ch == '/') ? '.' : ch;
  }  // for

  // Add the appropriate number of brackets for arrays.
  for (int j = 0; j < arrayDepth; j++) {
    newStr[i++] = '[';
    newStr[i++] = ']';
  }  // for

  newStr[i] = '\0';
  return newStr;
}

/*
 * Retrieves the class name portion of a type descriptor.
 */
static std::unique_ptr<char[]> descriptorClassToName(const char* str) {
  // Reduce to just the class name prefix.
  const char* lastSlash = strrchr(str, '/');
  if (lastSlash == nullptr) {
    lastSlash = str + 1;  // start past 'L'
  } else {
    lastSlash++;          // start past '/'
  }

  // Copy class name over, trimming trailing ';'.
  const int targetLen = strlen(lastSlash);
  std::unique_ptr<char[]> newStr(new char[targetLen]);
  for (int i = 0; i < targetLen - 1; i++) {
    newStr[i] = lastSlash[i];
  }  // for
  newStr[targetLen - 1] = '\0';
  return newStr;
}

/*
 * Returns string representing the boolean value.
 */
static const char* strBool(bool val) {
  return val ? "true" : "false";
}

/*
 * Returns a quoted string representing the boolean value.
 */
static const char* quotedBool(bool val) {
  return val ? "\"true\"" : "\"false\"";
}

/*
 * Returns a quoted string representing the access flags.
 */
static const char* quotedVisibility(u4 accessFlags) {
  if (accessFlags & kAccPublic) {
    return "\"public\"";
  } else if (accessFlags & kAccProtected) {
    return "\"protected\"";
  } else if (accessFlags & kAccPrivate) {
    return "\"private\"";
  } else {
    return "\"package\"";
  }
}

/*
 * Counts the number of '1' bits in a word.
 */
static int countOnes(u4 val) {
  val = val - ((val >> 1) & 0x55555555);
  val = (val & 0x33333333) + ((val >> 2) & 0x33333333);
  return (((val + (val >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
}

/*
 * Creates a new string with human-readable access flags.
 *
 * In the base language the access_flags fields are type u2; in Dalvik
 * they're u4.
 */
static char* createAccessFlagStr(u4 flags, AccessFor forWhat) {
  static constexpr const char* kAccessStrings[static_cast<int>(AccessFor::kCount)][kNumFlags] = {
    {
      "PUBLIC",                /* 0x00001 */
      "PRIVATE",               /* 0x00002 */
      "PROTECTED",             /* 0x00004 */
      "STATIC",                /* 0x00008 */
      "FINAL",                 /* 0x00010 */
      "?",                     /* 0x00020 */
      "?",                     /* 0x00040 */
      "?",                     /* 0x00080 */
      "?",                     /* 0x00100 */
      "INTERFACE",             /* 0x00200 */
      "ABSTRACT",              /* 0x00400 */
      "?",                     /* 0x00800 */
      "SYNTHETIC",             /* 0x01000 */
      "ANNOTATION",            /* 0x02000 */
      "ENUM",                  /* 0x04000 */
      "?",                     /* 0x08000 */
      "VERIFIED",              /* 0x10000 */
      "OPTIMIZED",             /* 0x20000 */
    }, {
      "PUBLIC",                /* 0x00001 */
      "PRIVATE",               /* 0x00002 */
      "PROTECTED",             /* 0x00004 */
      "STATIC",                /* 0x00008 */
      "FINAL",                 /* 0x00010 */
      "SYNCHRONIZED",          /* 0x00020 */
      "BRIDGE",                /* 0x00040 */
      "VARARGS",               /* 0x00080 */
      "NATIVE",                /* 0x00100 */
      "?",                     /* 0x00200 */
      "ABSTRACT",              /* 0x00400 */
      "STRICT",                /* 0x00800 */
      "SYNTHETIC",             /* 0x01000 */
      "?",                     /* 0x02000 */
      "?",                     /* 0x04000 */
      "MIRANDA",               /* 0x08000 */
      "CONSTRUCTOR",           /* 0x10000 */
      "DECLARED_SYNCHRONIZED", /* 0x20000 */
    }, {
      "PUBLIC",                /* 0x00001 */
      "PRIVATE",               /* 0x00002 */
      "PROTECTED",             /* 0x00004 */
      "STATIC",                /* 0x00008 */
      "FINAL",                 /* 0x00010 */
      "?",                     /* 0x00020 */
      "VOLATILE",              /* 0x00040 */
      "TRANSIENT",             /* 0x00080 */
      "?",                     /* 0x00100 */
      "?",                     /* 0x00200 */
      "?",                     /* 0x00400 */
      "?",                     /* 0x00800 */
      "SYNTHETIC",             /* 0x01000 */
      "?",                     /* 0x02000 */
      "ENUM",                  /* 0x04000 */
      "?",                     /* 0x08000 */
      "?",                     /* 0x10000 */
      "?",                     /* 0x20000 */
    },
  };

  // Allocate enough storage to hold the expected number of strings,
  // plus a space between each.  We over-allocate, using the longest
  // string above as the base metric.
  static constexpr int kLongest = 21;  // The strlen of longest string above.
  const int count = countOnes(flags);
  char* str;
  char* cp;
  cp = str = reinterpret_cast<char*>(malloc(count * (kLongest + 1) + 1));

  for (int i = 0; i < kNumFlags; i++) {
    if (flags & 0x01) {
      const char* accessStr = kAccessStrings[static_cast<int>(forWhat)][i];
      const int len = strlen(accessStr);
      if (cp != str) {
        *cp++ = ' ';
      }
      memcpy(cp, accessStr, len);
      cp += len;
    }
    flags >>= 1;
  }  // for

  *cp = '\0';
  return str;
}

/*
 * Copies character data from "data" to "out", converting non-ASCII values
 * to fprintf format chars or an ASCII filler ('.' or '?').
 *
 * The output buffer must be able to hold (2*len)+1 bytes.  The result is
 * NULL-terminated.
 */
static void asciify(char* out, const unsigned char* data, size_t len) {
  for (; len != 0u; --len) {
    if (*data < 0x20) {
      // Could do more here, but we don't need them yet.
      switch (*data) {
        case '\0':
          *out++ = '\\';
          *out++ = '0';
          break;
        case '\n':
          *out++ = '\\';
          *out++ = 'n';
          break;
        default:
          *out++ = '.';
          break;
      }  // switch
    } else if (*data >= 0x80) {
      *out++ = '?';
    } else {
      *out++ = *data;
    }
    data++;
  }  // while
  *out = '\0';
}

/* clang-format off */
constexpr char kEscapedLength[256] = {
    4, 4, 4, 4, 4, 4, 4, 4, 2, 2, 2, 4, 2, 2, 4, 4,  // \a, \b, \t, \n, \r
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // ",
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // '0'..'9'
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 'A'..'O'
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1,  // 'P'..'Z', '\'
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 'a'..'o'
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4,  // 'p'..'z', DEL
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // Unicode range, keep
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};
/* clang-format on */

/*
 * Check if a UTF8 string contains characters we should quote.
 */
static bool needsEscape(std::string_view s) {
  for (unsigned char c : s) {
    if (kEscapedLength[c] != 1) {
      return true;
    }
  }
  return false;
}

std::string escapeString(std::string_view s) {
  std::ostringstream oss;
  for (unsigned char c : s) {
    switch (kEscapedLength[c]) {
      case 1:
        oss << static_cast<char>(c);
        break;
      case 2:
        switch (c) {
          case '\b':
            oss << '\\' << 'b';
            break;
          case '\f':
            oss << '\\' << 'f';
            break;
          case '\n':
            oss << '\\' << 'n';
            break;
          case '\r':
            oss << '\\' << 'r';
            break;
          case '\t':
            oss << '\\' << 't';
            break;
          case '\"':
            oss << '\\' << '"';
            break;
          case '\\':
            oss << '\\' << '\\';
            break;
        }
        break;
      case 4:
        oss << '\\' << '0' + (c / 64) << '0' + ((c % 64) / 8) << '0' + (c % 8);
        break;
    }
  }
  return oss.str();
}

/*
 * Dumps a string value with some escape characters.
 */
static void dumpEscapedString(std::string_view s) {
  fputs("\"", gOutFile);
  if (needsEscape(s)) {
    std::string e = escapeString(s);
    fputs(e.c_str(), gOutFile);
  } else {
    for (char c : s) {
      fputc(c, gOutFile);
    }
  }
  fputs("\"", gOutFile);
}

static size_t utf8Bytes(char start_byte) {
  uint8_t sb = static_cast<uint8_t>(start_byte);
  if ((sb & 0x80) == 0) {
    return 1;
  }
  size_t msb = art::MostSignificantBit(static_cast<uint8_t>(~sb));
  CHECK_LE(7u - msb, 4u);
  return 7 - msb;
}

/*
 * Dumps a string as an XML attribute value.
 */
static void dumpXmlAttribute(std::string_view p) __attribute__((optnone)) {
  for (const char* c = p.begin(); c < p.end(); ++c) {
    if (std::isprint(*c)) {
      switch (*c) {
        case '&':
          fputs("&amp;", gOutFile);
          break;
        case '<':
          fputs("&lt;", gOutFile);
          break;
        case '>':
          fputs("&gt;", gOutFile);
          break;
        case '"':
          fputs("&quot;", gOutFile);
          break;
        case '\\':
          fputs("\\\\", gOutFile);
          break;
        default:
          putc(*c, gOutFile);
      }  // switch
    } else {
      uint32_t data = 0;
      size_t remaining;
      uint8_t uc = static_cast<uint8_t>(*c);
      if (((uc) & 0x80) == 0) {
        // Not a multi-byte char
        data = static_cast<uint32_t>(*c);
        remaining = 0;
      } else if (utf8Bytes(uc) == 2) {
        // 2 bytes
        data = ((uc) & 0b00011111);
        remaining = 1;
      } else if (utf8Bytes(uc) == 3) {
        // 3 bytes
        data = ((uc) & 0b00001111);
        remaining = 2;
      } else {
        // 4 bytes
        CHECK_EQ(utf8Bytes(uc), 4u);
        data = ((uc) & 0b00000111);
        remaining = 3;
      }
      for (size_t i = 0; i < remaining; ++i) {
        ++c;
        data = data << 6;
        uc = static_cast<uint8_t>(*c);
        data |= static_cast<uint32_t>(uc & 0b00111111u);
      }
      // No good option so just use java encoding, too many chars are invalid
      fprintf(gOutFile, "\\u%04x", data);
    }
  }  // for
}

/*
 * Reads variable width value, possibly sign extended at the last defined byte.
 */
static u8 readVarWidth(const u1** data, u1 arg, bool sign_extend) {
  u8 value = 0;
  for (u4 i = 0; i <= arg; i++) {
    value |= static_cast<u8>(*(*data)++) << (i * 8);
  }
  if (sign_extend) {
    int shift = (7 - arg) * 8;
    return (static_cast<s8>(value) << shift) >> shift;
  }
  return value;
}

/*
 * Dumps encoded value.
 */
static void dumpEncodedValue(const DexFile* pDexFile, const u1** data);  // forward
static void dumpEncodedValue(const DexFile* pDexFile, const u1** data, u1 type, u1 arg) {
  switch (type) {
    case DexFile::kDexAnnotationByte:
      fprintf(gOutFile, "%" PRId8, static_cast<s1>(readVarWidth(data, arg, false)));
      break;
    case DexFile::kDexAnnotationShort:
      fprintf(gOutFile, "%" PRId16, static_cast<s2>(readVarWidth(data, arg, true)));
      break;
    case DexFile::kDexAnnotationChar:
      fprintf(gOutFile, "%" PRIu16, static_cast<u2>(readVarWidth(data, arg, false)));
      break;
    case DexFile::kDexAnnotationInt:
      fprintf(gOutFile, "%" PRId32, static_cast<s4>(readVarWidth(data, arg, true)));
      break;
    case DexFile::kDexAnnotationLong:
      fprintf(gOutFile, "%" PRId64, static_cast<s8>(readVarWidth(data, arg, true)));
      break;
    case DexFile::kDexAnnotationFloat: {
      // Fill on right.
      union {
        float f;
        u4 data;
      } conv;
      conv.data = static_cast<u4>(readVarWidth(data, arg, false)) << (3 - arg) * 8;
      fprintf(gOutFile, "%g", conv.f);
      break;
    }
    case DexFile::kDexAnnotationDouble: {
      // Fill on right.
      union {
        double d;
        u8 data;
      } conv;
      conv.data = readVarWidth(data, arg, false) << (7 - arg) * 8;
      fprintf(gOutFile, "%g", conv.d);
      break;
    }
    case DexFile::kDexAnnotationMethodType: {
      const u4 proto_idx = static_cast<u4>(readVarWidth(data, arg, false));
      const dex::ProtoId& pProtoId = pDexFile->GetProtoId(dex::ProtoIndex(proto_idx));
      fputs(pDexFile->GetProtoSignature(pProtoId).ToString().c_str(), gOutFile);
      break;
    }
    case DexFile::kDexAnnotationMethodHandle: {
      const u4 method_handle_idx = static_cast<u4>(readVarWidth(data, arg, false));
      fprintf(gOutFile, "method_handle@%u", method_handle_idx);
      break;
    }
    case DexFile::kDexAnnotationString: {
      const u4 idx = static_cast<u4>(readVarWidth(data, arg, false));
      if (gOptions.outputFormat == OUTPUT_PLAIN) {
        dumpEscapedString(pDexFile->GetStringView(dex::StringIndex(idx)));
      } else {
        dumpXmlAttribute(pDexFile->GetStringView(dex::StringIndex(idx)));
      }
      break;
    }
    case DexFile::kDexAnnotationType: {
      const u4 str_idx = static_cast<u4>(readVarWidth(data, arg, false));
      fputs(pDexFile->GetTypeDescriptor(dex::TypeIndex(str_idx)), gOutFile);
      break;
    }
    case DexFile::kDexAnnotationField:
    case DexFile::kDexAnnotationEnum: {
      const u4 field_idx = static_cast<u4>(readVarWidth(data, arg, false));
      const dex::FieldId& pFieldId = pDexFile->GetFieldId(field_idx);
      fputs(pDexFile->GetStringData(pFieldId.name_idx_), gOutFile);
      break;
    }
    case DexFile::kDexAnnotationMethod: {
      const u4 method_idx = static_cast<u4>(readVarWidth(data, arg, false));
      const dex::MethodId& pMethodId = pDexFile->GetMethodId(method_idx);
      fputs(pDexFile->GetStringData(pMethodId.name_idx_), gOutFile);
      break;
    }
    case DexFile::kDexAnnotationArray: {
      fputc('{', gOutFile);
      // Decode and display all elements.
      const u4 size = DecodeUnsignedLeb128(data);
      for (u4 i = 0; i < size; i++) {
        fputc(' ', gOutFile);
        dumpEncodedValue(pDexFile, data);
      }
      fputs(" }", gOutFile);
      break;
    }
    case DexFile::kDexAnnotationAnnotation: {
      const u4 type_idx = DecodeUnsignedLeb128(data);
      fputs(pDexFile->GetTypeDescriptor(dex::TypeIndex(type_idx)), gOutFile);
      // Decode and display all name=value pairs.
      const u4 size = DecodeUnsignedLeb128(data);
      for (u4 i = 0; i < size; i++) {
        const u4 name_idx = DecodeUnsignedLeb128(data);
        fputc(' ', gOutFile);
        fputs(pDexFile->GetStringData(dex::StringIndex(name_idx)), gOutFile);
        fputc('=', gOutFile);
        dumpEncodedValue(pDexFile, data);
      }
      break;
    }
    case DexFile::kDexAnnotationNull:
      fputs("null", gOutFile);
      break;
    case DexFile::kDexAnnotationBoolean:
      fputs(strBool(arg), gOutFile);
      break;
    default:
      fputs("????", gOutFile);
      break;
  }  // switch
}

/*
 * Dumps encoded value with prefix.
 */
static void dumpEncodedValue(const DexFile* pDexFile, const u1** data) {
  const u1 enc = *(*data)++;
  dumpEncodedValue(pDexFile, data, enc & 0x1f, enc >> 5);
}

/*
 * Dumps the file header.
 */
static void dumpFileHeader(const DexFile* pDexFile) {
  const DexFile::Header& pHeader = pDexFile->GetHeader();
  char sanitized[sizeof(pHeader.magic_) * 2 + 1];
  fprintf(gOutFile, "DEX file header:\n");
  asciify(sanitized, pHeader.magic_.data(), pHeader.magic_.size());
  fprintf(gOutFile, "magic               : '%s'\n", sanitized);
  fprintf(gOutFile, "checksum            : %08x\n", pHeader.checksum_);
  fprintf(gOutFile, "signature           : %02x%02x...%02x%02x\n",
          pHeader.signature_[0], pHeader.signature_[1],
          pHeader.signature_[DexFile::kSha1DigestSize - 2],
          pHeader.signature_[DexFile::kSha1DigestSize - 1]);
  fprintf(gOutFile, "file_size           : %d\n", pHeader.file_size_);
  fprintf(gOutFile, "header_size         : %d\n", pHeader.header_size_);
  fprintf(gOutFile, "link_size           : %d\n", pHeader.link_size_);
  fprintf(gOutFile, "link_off            : %d (0x%06x)\n",
          pHeader.link_off_, pHeader.link_off_);
  fprintf(gOutFile, "string_ids_size     : %d\n", pHeader.string_ids_size_);
  fprintf(gOutFile, "string_ids_off      : %d (0x%06x)\n",
          pHeader.string_ids_off_, pHeader.string_ids_off_);
  fprintf(gOutFile, "type_ids_size       : %d\n", pHeader.type_ids_size_);
  fprintf(gOutFile, "type_ids_off        : %d (0x%06x)\n",
          pHeader.type_ids_off_, pHeader.type_ids_off_);
  fprintf(gOutFile, "proto_ids_size      : %d\n", pHeader.proto_ids_size_);
  fprintf(gOutFile, "proto_ids_off       : %d (0x%06x)\n",
          pHeader.proto_ids_off_, pHeader.proto_ids_off_);
  fprintf(gOutFile, "field_ids_size      : %d\n", pHeader.field_ids_size_);
  fprintf(gOutFile, "field_ids_off       : %d (0x%06x)\n",
          pHeader.field_ids_off_, pHeader.field_ids_off_);
  fprintf(gOutFile, "method_ids_size     : %d\n", pHeader.method_ids_size_);
  fprintf(gOutFile, "method_ids_off      : %d (0x%06x)\n",
          pHeader.method_ids_off_, pHeader.method_ids_off_);
  fprintf(gOutFile, "class_defs_size     : %d\n", pHeader.class_defs_size_);
  fprintf(gOutFile, "class_defs_off      : %d (0x%06x)\n",
          pHeader.class_defs_off_, pHeader.class_defs_off_);
  fprintf(gOutFile, "data_size           : %d\n", pHeader.data_size_);
  fprintf(gOutFile, "data_off            : %d (0x%06x)\n\n",
          pHeader.data_off_, pHeader.data_off_);
}

/*
 * Dumps a class_def_item.
 */
static void dumpClassDef(const DexFile* pDexFile, int idx) {
  // General class information.
  const dex::ClassDef& pClassDef = pDexFile->GetClassDef(idx);
  fprintf(gOutFile, "Class #%d header:\n", idx);
  fprintf(gOutFile, "class_idx           : %d\n", pClassDef.class_idx_.index_);
  fprintf(gOutFile, "access_flags        : %d (0x%04x)\n",
          pClassDef.access_flags_, pClassDef.access_flags_);
  fprintf(gOutFile, "superclass_idx      : %d\n", pClassDef.superclass_idx_.index_);
  fprintf(gOutFile, "interfaces_off      : %d (0x%06x)\n",
          pClassDef.interfaces_off_, pClassDef.interfaces_off_);
  fprintf(gOutFile, "source_file_idx     : %d\n", pClassDef.source_file_idx_.index_);
  fprintf(gOutFile, "annotations_off     : %d (0x%06x)\n",
          pClassDef.annotations_off_, pClassDef.annotations_off_);
  fprintf(gOutFile, "class_data_off      : %d (0x%06x)\n",
          pClassDef.class_data_off_, pClassDef.class_data_off_);

  // Fields and methods.
  ClassAccessor accessor(*pDexFile, idx);
  fprintf(gOutFile, "static_fields_size  : %d\n", accessor.NumStaticFields());
  fprintf(gOutFile, "instance_fields_size: %d\n", accessor.NumInstanceFields());
  fprintf(gOutFile, "direct_methods_size : %d\n", accessor.NumDirectMethods());
  fprintf(gOutFile, "virtual_methods_size: %d\n", accessor.NumVirtualMethods());
  fprintf(gOutFile, "\n");
}

/**
 * Dumps an annotation set item.
 */
static void dumpAnnotationSetItem(const DexFile* pDexFile, const dex::AnnotationSetItem* set_item) {
  if (set_item == nullptr || set_item->size_ == 0) {
    fputs("  empty-annotation-set\n", gOutFile);
    return;
  }
  for (u4 i = 0; i < set_item->size_; i++) {
    const dex::AnnotationItem* annotation = pDexFile->GetAnnotationItem(set_item, i);
    if (annotation == nullptr) {
      continue;
    }
    fputs("  ", gOutFile);
    switch (annotation->visibility_) {
      case DexFile::kDexVisibilityBuild:   fputs("VISIBILITY_BUILD ",   gOutFile); break;
      case DexFile::kDexVisibilityRuntime: fputs("VISIBILITY_RUNTIME ", gOutFile); break;
      case DexFile::kDexVisibilitySystem:  fputs("VISIBILITY_SYSTEM ",  gOutFile); break;
      default:                             fputs("VISIBILITY_UNKNOWN ", gOutFile); break;
    }  // switch
    // Decode raw bytes in annotation.
    const u1* rData = annotation->annotation_;
    dumpEncodedValue(pDexFile, &rData, DexFile::kDexAnnotationAnnotation, 0);
    fputc('\n', gOutFile);
  }
}

/*
 * Dumps class annotations.
 */
static void dumpClassAnnotations(const DexFile* pDexFile, int idx) {
  const dex::ClassDef& pClassDef = pDexFile->GetClassDef(idx);
  const dex::AnnotationsDirectoryItem* dir = pDexFile->GetAnnotationsDirectory(pClassDef);
  if (dir == nullptr) {
    return;  // none
  }

  fprintf(gOutFile, "Class #%d annotations:\n", idx);

  const dex::AnnotationSetItem* class_set_item = pDexFile->GetClassAnnotationSet(dir);
  const dex::FieldAnnotationsItem* fields = pDexFile->GetFieldAnnotations(dir);
  const dex::MethodAnnotationsItem* methods = pDexFile->GetMethodAnnotations(dir);
  const dex::ParameterAnnotationsItem* pars = pDexFile->GetParameterAnnotations(dir);

  // Annotations on the class itself.
  if (class_set_item != nullptr) {
    fprintf(gOutFile, "Annotations on class\n");
    dumpAnnotationSetItem(pDexFile, class_set_item);
  }

  // Annotations on fields.
  if (fields != nullptr) {
    for (u4 i = 0; i < dir->fields_size_; i++) {
      const u4 field_idx = fields[i].field_idx_;
      const dex::FieldId& pFieldId = pDexFile->GetFieldId(field_idx);
      const char* field_name = pDexFile->GetStringData(pFieldId.name_idx_);
      fprintf(gOutFile, "Annotations on field #%u '%s'\n", field_idx, field_name);
      dumpAnnotationSetItem(pDexFile, pDexFile->GetFieldAnnotationSetItem(fields[i]));
    }
  }

  // Annotations on methods.
  if (methods != nullptr) {
    for (u4 i = 0; i < dir->methods_size_; i++) {
      const u4 method_idx = methods[i].method_idx_;
      const dex::MethodId& pMethodId = pDexFile->GetMethodId(method_idx);
      const char* method_name = pDexFile->GetStringData(pMethodId.name_idx_);
      fprintf(gOutFile, "Annotations on method #%u '%s'\n", method_idx, method_name);
      dumpAnnotationSetItem(pDexFile, pDexFile->GetMethodAnnotationSetItem(methods[i]));
    }
  }

  // Annotations on method parameters.
  if (pars != nullptr) {
    for (u4 i = 0; i < dir->parameters_size_; i++) {
      const u4 method_idx = pars[i].method_idx_;
      const dex::MethodId& pMethodId = pDexFile->GetMethodId(method_idx);
      const char* method_name = pDexFile->GetStringData(pMethodId.name_idx_);
      fprintf(gOutFile, "Annotations on method #%u '%s' parameters\n", method_idx, method_name);
      const dex::AnnotationSetRefList*
          list = pDexFile->GetParameterAnnotationSetRefList(&pars[i]);
      if (list != nullptr) {
        for (u4 j = 0; j < list->size_; j++) {
          fprintf(gOutFile, "#%u\n", j);
          dumpAnnotationSetItem(pDexFile, pDexFile->GetSetRefItemItem(&list->list_[j]));
        }
      }
    }
  }

  fputc('\n', gOutFile);
}

/*
 * Dumps an interface that a class declares to implement.
 */
static void dumpInterface(const DexFile* pDexFile, const dex::TypeItem& pTypeItem, int i) {
  const char* interfaceName = pDexFile->GetTypeDescriptor(pTypeItem.type_idx_);
  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "    #%d              : '%s'\n", i, interfaceName);
  } else {
    std::unique_ptr<char[]> dot(descriptorToDot(interfaceName));
    fprintf(gOutFile, "<implements name=\"%s\">\n</implements>\n", dot.get());
  }
}

/*
 * Dumps the catches table associated with the code.
 */
static void dumpCatches(const DexFile* pDexFile, const dex::CodeItem* pCode) {
  CodeItemDataAccessor accessor(*pDexFile, pCode);
  const u4 triesSize = accessor.TriesSize();

  // No catch table.
  if (triesSize == 0) {
    fprintf(gOutFile, "      catches       : (none)\n");
    return;
  }

  // Dump all table entries.
  fprintf(gOutFile, "      catches       : %d\n", triesSize);
  for (const dex::TryItem& try_item : accessor.TryItems()) {
    const u4 start = try_item.start_addr_;
    const u4 end = start + try_item.insn_count_;
    fprintf(gOutFile, "        0x%04x - 0x%04x\n", start, end);
    for (CatchHandlerIterator it(accessor, try_item); it.HasNext(); it.Next()) {
      const dex::TypeIndex tidx = it.GetHandlerTypeIndex();
      const char* descriptor = (!tidx.IsValid()) ? "<any>" : pDexFile->GetTypeDescriptor(tidx);
      fprintf(gOutFile, "          %s -> 0x%04x\n", descriptor, it.GetHandlerAddress());
    }  // for
  }  // for
}

/*
 * Helper for dumpInstruction(), which builds the string
 * representation for the index in the given instruction.
 * Returns a pointer to a buffer of sufficient size.
 */
static std::unique_ptr<char[]> indexString(const DexFile* pDexFile,
                                           const Instruction* pDecInsn,
                                           size_t bufSize) {
  std::unique_ptr<char[]> buf(new char[bufSize]);
  // Determine index and width of the string.
  u4 index = 0;
  u2 secondary_index = 0;
  u4 width = 4;
  switch (Instruction::FormatOf(pDecInsn->Opcode())) {
    // SOME NOT SUPPORTED:
    // case Instruction::k20bc:
    case Instruction::k21c:
    case Instruction::k35c:
    // case Instruction::k35ms:
    case Instruction::k3rc:
    // case Instruction::k3rms:
    // case Instruction::k35mi:
    // case Instruction::k3rmi:
      index = pDecInsn->VRegB();
      width = 4;
      break;
    case Instruction::k31c:
      index = pDecInsn->VRegB();
      width = 8;
      break;
    case Instruction::k22c:
    // case Instruction::k22cs:
      index = pDecInsn->VRegC();
      width = 4;
      break;
    case Instruction::k45cc:
    case Instruction::k4rcc:
      index = pDecInsn->VRegB();
      secondary_index = pDecInsn->VRegH();
      width = 4;
      break;
    default:
      break;
  }  // switch

  // Determine index type.
  size_t outSize = 0;
  switch (Instruction::IndexTypeOf(pDecInsn->Opcode())) {
    case Instruction::kIndexUnknown:
      // This function should never get called for this type, but do
      // something sensible here, just to help with debugging.
      outSize = snprintf(buf.get(), bufSize, "<unknown-index>");
      break;
    case Instruction::kIndexNone:
      // This function should never get called for this type, but do
      // something sensible here, just to help with debugging.
      outSize = snprintf(buf.get(), bufSize, "<no-index>");
      break;
    case Instruction::kIndexTypeRef:
      if (index < pDexFile->GetHeader().type_ids_size_) {
        const char* tp = pDexFile->GetTypeDescriptor(dex::TypeIndex(index));
        outSize = snprintf(buf.get(), bufSize, "%s // type@%0*x", tp, width, index);
      } else {
        outSize = snprintf(buf.get(), bufSize, "<type?> // type@%0*x", width, index);
      }
      break;
    case Instruction::kIndexStringRef:
      if (index < pDexFile->GetHeader().string_ids_size_) {
        const char* st = pDexFile->GetStringData(dex::StringIndex(index));
        if (needsEscape(std::string_view(st))) {
          std::string escaped = escapeString(st);
          outSize =
              snprintf(buf.get(), bufSize, "\"%s\" // string@%0*x", escaped.c_str(), width, index);
        } else {
          outSize = snprintf(buf.get(), bufSize, "\"%s\" // string@%0*x", st, width, index);
        }
      } else {
        outSize = snprintf(buf.get(), bufSize, "<string?> // string@%0*x", width, index);
      }
      break;
    case Instruction::kIndexMethodRef:
      if (index < pDexFile->GetHeader().method_ids_size_) {
        const dex::MethodId& pMethodId = pDexFile->GetMethodId(index);
        const char* name = pDexFile->GetStringData(pMethodId.name_idx_);
        const Signature signature = pDexFile->GetMethodSignature(pMethodId);
        const char* backDescriptor = pDexFile->GetTypeDescriptor(pMethodId.class_idx_);
        outSize = snprintf(buf.get(), bufSize, "%s.%s:%s // method@%0*x",
                           backDescriptor, name, signature.ToString().c_str(), width, index);
      } else {
        outSize = snprintf(buf.get(), bufSize, "<method?> // method@%0*x", width, index);
      }
      break;
    case Instruction::kIndexFieldRef:
      if (index < pDexFile->GetHeader().field_ids_size_) {
        const dex::FieldId& pFieldId = pDexFile->GetFieldId(index);
        const char* name = pDexFile->GetStringData(pFieldId.name_idx_);
        const char* typeDescriptor = pDexFile->GetTypeDescriptor(pFieldId.type_idx_);
        const char* backDescriptor = pDexFile->GetTypeDescriptor(pFieldId.class_idx_);
        outSize = snprintf(buf.get(), bufSize, "%s.%s:%s // field@%0*x",
                           backDescriptor, name, typeDescriptor, width, index);
      } else {
        outSize = snprintf(buf.get(), bufSize, "<field?> // field@%0*x", width, index);
      }
      break;
    case Instruction::kIndexMethodAndProtoRef: {
      std::string method("<method?>");
      std::string proto("<proto?>");
      if (index < pDexFile->GetHeader().method_ids_size_) {
        const dex::MethodId& pMethodId = pDexFile->GetMethodId(index);
        const char* name = pDexFile->GetStringData(pMethodId.name_idx_);
        const Signature signature = pDexFile->GetMethodSignature(pMethodId);
        const char* backDescriptor = pDexFile->GetTypeDescriptor(pMethodId.class_idx_);
        method = android::base::StringPrintf("%s.%s:%s",
                                             backDescriptor,
                                             name,
                                             signature.ToString().c_str());
      }
      if (secondary_index < pDexFile->GetHeader().proto_ids_size_) {
        const dex::ProtoId& protoId = pDexFile->GetProtoId(dex::ProtoIndex(secondary_index));
        const Signature signature = pDexFile->GetProtoSignature(protoId);
        proto = signature.ToString();
      }
      outSize = snprintf(buf.get(), bufSize, "%s, %s // method@%0*x, proto@%0*x",
                         method.c_str(), proto.c_str(), width, index, width, secondary_index);
      break;
    }
    case Instruction::kIndexCallSiteRef:
      // Call site information is too large to detail in disassembly so just output the index.
      outSize = snprintf(buf.get(), bufSize, "call_site@%0*x", width, index);
      break;
    case Instruction::kIndexMethodHandleRef:
      // Method handle information is too large to detail in disassembly so just output the index.
      outSize = snprintf(buf.get(), bufSize, "method_handle@%0*x", width, index);
      break;
    case Instruction::kIndexProtoRef:
      if (index < pDexFile->GetHeader().proto_ids_size_) {
        const dex::ProtoId& protoId = pDexFile->GetProtoId(dex::ProtoIndex(index));
        const Signature signature = pDexFile->GetProtoSignature(protoId);
        const std::string& proto = signature.ToString();
        outSize = snprintf(buf.get(), bufSize, "%s // proto@%0*x", proto.c_str(), width, index);
      } else {
        outSize = snprintf(buf.get(), bufSize, "<?> // proto@%0*x", width, index);
      }
      break;
  }  // switch

  if (outSize == 0) {
    // The index type has not been handled in the switch above.
    outSize = snprintf(buf.get(), bufSize, "<?>");
  }

  // Determine success of string construction.
  if (outSize >= bufSize) {
    // The buffer wasn't big enough; retry with computed size. Note: snprintf()
    // doesn't count/ the '\0' as part of its returned size, so we add explicit
    // space for it here.
    return indexString(pDexFile, pDecInsn, outSize + 1);
  }
  return buf;
}

/*
 * Dumps a single instruction.
 */
static void dumpInstruction(const DexFile* pDexFile,
                            const dex::CodeItem* pCode,
                            u4 codeOffset, u4 insnIdx, u4 insnWidth,
                            const Instruction* pDecInsn) {
  // Address of instruction (expressed as byte offset).
  fprintf(gOutFile, "%06x:", codeOffset + 0x10 + insnIdx * 2);

  // Dump (part of) raw bytes.
  CodeItemInstructionAccessor accessor(*pDexFile, pCode);
  for (u4 i = 0; i < 8; i++) {
    if (i < insnWidth) {
      if (i == 7) {
        fprintf(gOutFile, " ... ");
      } else {
        // Print 16-bit value in little-endian order.
        const u1* bytePtr = (const u1*) &accessor.Insns()[insnIdx + i];
        fprintf(gOutFile, " %02x%02x", bytePtr[0], bytePtr[1]);
      }
    } else {
      fputs("     ", gOutFile);
    }
  }  // for

  // Dump pseudo-instruction or opcode.
  if (pDecInsn->Opcode() == Instruction::NOP) {
    const u2 instr = get2LE((const u1*) &accessor.Insns()[insnIdx]);
    if (instr == Instruction::kPackedSwitchSignature) {
      fprintf(gOutFile, "|%04x: packed-switch-data (%d units)", insnIdx, insnWidth);
    } else if (instr == Instruction::kSparseSwitchSignature) {
      fprintf(gOutFile, "|%04x: sparse-switch-data (%d units)", insnIdx, insnWidth);
    } else if (instr == Instruction::kArrayDataSignature) {
      fprintf(gOutFile, "|%04x: array-data (%d units)", insnIdx, insnWidth);
    } else {
      fprintf(gOutFile, "|%04x: nop // spacer", insnIdx);
    }
  } else {
    fprintf(gOutFile, "|%04x: %s", insnIdx, pDecInsn->Name());
  }

  // Set up additional argument.
  std::unique_ptr<char[]> indexBuf;
  if (Instruction::IndexTypeOf(pDecInsn->Opcode()) != Instruction::kIndexNone) {
    indexBuf = indexString(pDexFile, pDecInsn, 200);
  }

  // Dump the instruction.
  //
  // NOTE: pDecInsn->DumpString(pDexFile) differs too much from original.
  //
  switch (Instruction::FormatOf(pDecInsn->Opcode())) {
    case Instruction::k10x:        // op
      break;
    case Instruction::k12x:        // op vA, vB
      fprintf(gOutFile, " v%d, v%d", pDecInsn->VRegA(), pDecInsn->VRegB());
      break;
    case Instruction::k11n:        // op vA, #+B
      fprintf(gOutFile, " v%d, #int %d // #%x",
              pDecInsn->VRegA(), (s4) pDecInsn->VRegB(), (u1)pDecInsn->VRegB());
      break;
    case Instruction::k11x:        // op vAA
      fprintf(gOutFile, " v%d", pDecInsn->VRegA());
      break;
    case Instruction::k10t:        // op +AA
    case Instruction::k20t: {      // op +AAAA
      const s4 targ = (s4) pDecInsn->VRegA();
      fprintf(gOutFile, " %04x // %c%04x",
              insnIdx + targ,
              (targ < 0) ? '-' : '+',
              (targ < 0) ? -targ : targ);
      break;
    }
    case Instruction::k22x:        // op vAA, vBBBB
      fprintf(gOutFile, " v%d, v%d", pDecInsn->VRegA(), pDecInsn->VRegB());
      break;
    case Instruction::k21t: {     // op vAA, +BBBB
      const s4 targ = (s4) pDecInsn->VRegB();
      fprintf(gOutFile, " v%d, %04x // %c%04x", pDecInsn->VRegA(),
              insnIdx + targ,
              (targ < 0) ? '-' : '+',
              (targ < 0) ? -targ : targ);
      break;
    }
    case Instruction::k21s:        // op vAA, #+BBBB
      fprintf(gOutFile, " v%d, #int %d // #%x",
              pDecInsn->VRegA(), (s4) pDecInsn->VRegB(), (u2)pDecInsn->VRegB());
      break;
    case Instruction::k21h:        // op vAA, #+BBBB0000[00000000]
      // The printed format varies a bit based on the actual opcode.
      if (pDecInsn->Opcode() == Instruction::CONST_HIGH16) {
        const s4 value = pDecInsn->VRegB() << 16;
        fprintf(gOutFile, " v%d, #int %d // #%x",
                pDecInsn->VRegA(), value, (u2) pDecInsn->VRegB());
      } else {
        const s8 value = ((s8) pDecInsn->VRegB()) << 48;
        fprintf(gOutFile, " v%d, #long %" PRId64 " // #%x",
                pDecInsn->VRegA(), value, (u2) pDecInsn->VRegB());
      }
      break;
    case Instruction::k21c:        // op vAA, thing@BBBB
    case Instruction::k31c:        // op vAA, thing@BBBBBBBB
      fprintf(gOutFile, " v%d, %s", pDecInsn->VRegA(), indexBuf.get());
      break;
    case Instruction::k23x:        // op vAA, vBB, vCC
      fprintf(gOutFile, " v%d, v%d, v%d",
              pDecInsn->VRegA(), pDecInsn->VRegB(), pDecInsn->VRegC());
      break;
    case Instruction::k22b:        // op vAA, vBB, #+CC
      fprintf(gOutFile, " v%d, v%d, #int %d // #%02x",
              pDecInsn->VRegA(), pDecInsn->VRegB(),
              (s4) pDecInsn->VRegC(), (u1) pDecInsn->VRegC());
      break;
    case Instruction::k22t: {      // op vA, vB, +CCCC
      const s4 targ = (s4) pDecInsn->VRegC();
      fprintf(gOutFile, " v%d, v%d, %04x // %c%04x",
              pDecInsn->VRegA(), pDecInsn->VRegB(),
              insnIdx + targ,
              (targ < 0) ? '-' : '+',
              (targ < 0) ? -targ : targ);
      break;
    }
    case Instruction::k22s:        // op vA, vB, #+CCCC
      fprintf(gOutFile, " v%d, v%d, #int %d // #%04x",
              pDecInsn->VRegA(), pDecInsn->VRegB(),
              (s4) pDecInsn->VRegC(), (u2) pDecInsn->VRegC());
      break;
    case Instruction::k22c:        // op vA, vB, thing@CCCC
    // NOT SUPPORTED:
    // case Instruction::k22cs:    // [opt] op vA, vB, field offset CCCC
      fprintf(gOutFile, " v%d, v%d, %s",
              pDecInsn->VRegA(), pDecInsn->VRegB(), indexBuf.get());
      break;
    case Instruction::k30t:
      fprintf(gOutFile, " #%08x", pDecInsn->VRegA());
      break;
    case Instruction::k31i: {     // op vAA, #+BBBBBBBB
      // This is often, but not always, a float.
      union {
        float f;
        u4 i;
      } conv;
      conv.i = pDecInsn->VRegB();
      fprintf(gOutFile, " v%d, #float %g // #%08x",
              pDecInsn->VRegA(), conv.f, pDecInsn->VRegB());
      break;
    }
    case Instruction::k31t:       // op vAA, offset +BBBBBBBB
      fprintf(gOutFile, " v%d, %08x // +%08x",
              pDecInsn->VRegA(), insnIdx + pDecInsn->VRegB(), pDecInsn->VRegB());
      break;
    case Instruction::k32x:        // op vAAAA, vBBBB
      fprintf(gOutFile, " v%d, v%d", pDecInsn->VRegA(), pDecInsn->VRegB());
      break;
    case Instruction::k35c:       // op {vC, vD, vE, vF, vG}, thing@BBBB
    case Instruction::k45cc: {    // op {vC, vD, vE, vF, vG}, method@BBBB, proto@HHHH
    // NOT SUPPORTED:
    // case Instruction::k35ms:       // [opt] invoke-virtual+super
    // case Instruction::k35mi:       // [opt] inline invoke
      u4 arg[Instruction::kMaxVarArgRegs];
      pDecInsn->GetVarArgs(arg);
      fputs(" {", gOutFile);
      for (int i = 0, n = pDecInsn->VRegA(); i < n; i++) {
        if (i == 0) {
          fprintf(gOutFile, "v%d", arg[i]);
        } else {
          fprintf(gOutFile, ", v%d", arg[i]);
        }
      }  // for
      fprintf(gOutFile, "}, %s", indexBuf.get());
      break;
    }
    case Instruction::k3rc:        // op {vCCCC .. v(CCCC+AA-1)}, thing@BBBB
    case Instruction::k4rcc: {     // op {vCCCC .. v(CCCC+AA-1)}, method@BBBB, proto@HHHH
    // NOT SUPPORTED:
    // case Instruction::k3rms:       // [opt] invoke-virtual+super/range
    // case Instruction::k3rmi:       // [opt] execute-inline/range
        // This doesn't match the "dx" output when some of the args are
        // 64-bit values -- dx only shows the first register.
        fputs(" {", gOutFile);
        for (int i = 0, n = pDecInsn->VRegA(); i < n; i++) {
          if (i == 0) {
            fprintf(gOutFile, "v%d", pDecInsn->VRegC() + i);
          } else {
            fprintf(gOutFile, ", v%d", pDecInsn->VRegC() + i);
          }
        }  // for
        fprintf(gOutFile, "}, %s", indexBuf.get());
      }
      break;
    case Instruction::k51l: {      // op vAA, #+BBBBBBBBBBBBBBBB
      // This is often, but not always, a double.
      union {
        double d;
        u8 j;
      } conv;
      conv.j = pDecInsn->WideVRegB();
      fprintf(gOutFile, " v%d, #double %g // #%016" PRIx64,
              pDecInsn->VRegA(), conv.d, pDecInsn->WideVRegB());
      break;
    }
    // NOT SUPPORTED:
    // case Instruction::k00x:        // unknown op or breakpoint
    //    break;
    default:
      fprintf(gOutFile, " ???");
      break;
  }  // switch

  fputc('\n', gOutFile);
}

/*
 * Dumps a bytecode disassembly.
 */
static void dumpBytecodes(const DexFile* pDexFile, u4 idx,
                          const dex::CodeItem* pCode, u4 codeOffset) {
  const dex::MethodId& pMethodId = pDexFile->GetMethodId(idx);
  const char* name = pDexFile->GetStringData(pMethodId.name_idx_);
  const Signature signature = pDexFile->GetMethodSignature(pMethodId);
  const char* backDescriptor = pDexFile->GetTypeDescriptor(pMethodId.class_idx_);

  // Generate header.
  std::unique_ptr<char[]> dot(descriptorToDot(backDescriptor));
  fprintf(gOutFile, "%06x:                                        |[%06x] %s.%s:%s\n",
          codeOffset, codeOffset, dot.get(), name, signature.ToString().c_str());

  // Iterate over all instructions.
  CodeItemDataAccessor accessor(*pDexFile, pCode);
  const u4 maxPc = accessor.InsnsSizeInCodeUnits();
  for (const DexInstructionPcPair& pair : accessor) {
    const u4 dexPc = pair.DexPc();
    if (dexPc >= maxPc) {
      LOG(WARNING) << "GLITCH: run-away instruction at idx=0x" << std::hex << dexPc;
      break;
    }
    const Instruction* instruction = &pair.Inst();
    const u4 insnWidth = instruction->SizeInCodeUnits();
    if (insnWidth == 0) {
      LOG(WARNING) << "GLITCH: zero-width instruction at idx=0x" << std::hex << dexPc;
      break;
    }
    dumpInstruction(pDexFile, pCode, codeOffset, dexPc, insnWidth, instruction);
  }  // for
}

static u4 findLastInstructionAddress(const CodeItemDebugInfoAccessor& accessor) {
  const u4 maxAddress = accessor.InsnsSizeInCodeUnits();
  u4 lastInstructionSize = 0;
  for (const DexInstructionPcPair& pair : accessor) {
    const u4 address = pair.DexPc();
    if (address >= maxAddress) {
      return 1;
    }
    lastInstructionSize = pair.Inst().SizeInCodeUnits();
  }
  return maxAddress - lastInstructionSize;
}

/*
 * Dumps code of a method.
 */
static void dumpCode(const DexFile* pDexFile, u4 idx, u4 flags,
                     const dex::CodeItem* pCode, u4 codeOffset) {
  CodeItemDebugInfoAccessor accessor(*pDexFile, pCode, idx);

  fprintf(gOutFile, "      registers     : %d\n", accessor.RegistersSize());
  fprintf(gOutFile, "      ins           : %d\n", accessor.InsSize());
  fprintf(gOutFile, "      outs          : %d\n", accessor.OutsSize());
  fprintf(gOutFile, "      insns size    : %d 16-bit code units\n",
          accessor.InsnsSizeInCodeUnits());

  // Bytecode disassembly, if requested.
  if (gOptions.disassemble) {
    dumpBytecodes(pDexFile, idx, pCode, codeOffset);
  }

  // Try-catch blocks.
  dumpCatches(pDexFile, pCode);

  if (gOptions.showDebugInfo) {
    const u4 lastInstructionAddress = findLastInstructionAddress(accessor);
    // Positions and locals table in the debug info.
    bool is_static = (flags & kAccStatic) != 0;
    fprintf(gOutFile, "      positions     :\n");
    accessor.DecodeDebugPositionInfo([&](const DexFile::PositionInfo& entry) {
      if (entry.address_ > lastInstructionAddress) {
        return true;
      } else {
        fprintf(gOutFile, "        0x%04x line=%d\n", entry.address_, entry.line_);
        return false;
      }
    });
    fprintf(gOutFile, "      locals        :\n");
    accessor.DecodeDebugLocalInfo(is_static,
                                  idx,
                                  [&](const DexFile::LocalInfo& entry) {
      fprintf(gOutFile,
              "        0x%04x - 0x%04x reg=%d %s %s",
              entry.start_address_,
              entry.end_address_,
              entry.reg_,
              entry.name_,
              entry.descriptor_);
      if (entry.signature_) {
        fputc(' ', gOutFile);
        fputs(entry.signature_, gOutFile);
      }
      fputc('\n', gOutFile);
    });
  }
}

static std::string GetHiddenapiFlagStr(uint32_t hiddenapi_flags) {
  std::stringstream ss;
  hiddenapi::ApiList api_list = hiddenapi::ApiList::FromDexFlags(hiddenapi_flags);
  api_list.Dump(ss);
  std::string str_api_list = ss.str();
  std::transform(str_api_list.begin(), str_api_list.end(), str_api_list.begin(), ::toupper);
  return str_api_list;
}

/*
 * Dumps a method.
 */
static void dumpMethod(const ClassAccessor::Method& method, int i) {
  // Bail for anything private if export only requested.
  const uint32_t flags = method.GetAccessFlags();
  if (gOptions.exportsOnly && (flags & (kAccPublic | kAccProtected)) == 0) {
    return;
  }

  const DexFile& dex_file = method.GetDexFile();
  const dex::MethodId& pMethodId = dex_file.GetMethodId(method.GetIndex());
  const char* name = dex_file.GetStringData(pMethodId.name_idx_);
  const Signature signature = dex_file.GetMethodSignature(pMethodId);
  char* typeDescriptor = strdup(signature.ToString().c_str());
  const char* backDescriptor = dex_file.GetTypeDescriptor(pMethodId.class_idx_);
  char* accessStr = createAccessFlagStr(flags, AccessFor::kMethod);
  const uint32_t hiddenapiFlags = method.GetHiddenapiFlags();

  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "    #%d              : (in %s)\n", i, backDescriptor);
    fprintf(gOutFile, "      name          : '%s'\n", name);
    fprintf(gOutFile, "      type          : '%s'\n", typeDescriptor);
    fprintf(gOutFile, "      access        : 0x%04x (%s)\n", flags, accessStr);
    if (gOptions.showSectionHeaders) {
      fprintf(gOutFile, "      method_idx    : %d\n", method.GetIndex());
    }
    if (hiddenapiFlags != 0u) {
      fprintf(gOutFile,
              "      hiddenapi     : 0x%04x (%s)\n",
              hiddenapiFlags,
              GetHiddenapiFlagStr(hiddenapiFlags).c_str());
    }
    if (method.GetCodeItem() == nullptr) {
      fprintf(gOutFile, "      code          : (none)\n");
    } else {
      fprintf(gOutFile, "      code          -\n");
      dumpCode(&dex_file,
               method.GetIndex(),
               flags,
               method.GetCodeItem(),
               method.GetCodeItemOffset());
    }
    if (gOptions.disassemble) {
      fputc('\n', gOutFile);
    }
  } else if (gOptions.outputFormat == OUTPUT_XML) {
    const bool constructor = (name[0] == '<');

    // Method name and prototype.
    if (constructor) {
      std::unique_ptr<char[]> dot(descriptorClassToName(backDescriptor));
      fprintf(gOutFile, "<constructor name=\"%s\"\n", dot.get());
      dot = descriptorToDot(backDescriptor);
      fprintf(gOutFile, " type=\"%s\"\n", dot.get());
    } else {
      fprintf(gOutFile, "<method name=\"%s\"\n", name);
      const char* returnType = strrchr(typeDescriptor, ')');
      if (returnType == nullptr) {
        LOG(ERROR) << "bad method type descriptor '" << typeDescriptor << "'";
        goto bail;
      }
      std::unique_ptr<char[]> dot(descriptorToDot(returnType + 1));
      fprintf(gOutFile, " return=\"%s\"\n", dot.get());
      fprintf(gOutFile, " abstract=%s\n", quotedBool((flags & kAccAbstract) != 0));
      fprintf(gOutFile, " native=%s\n", quotedBool((flags & kAccNative) != 0));
      fprintf(gOutFile, " synchronized=%s\n", quotedBool(
          (flags & (kAccSynchronized | kAccDeclaredSynchronized)) != 0));
    }

    // Additional method flags.
    fprintf(gOutFile, " static=%s\n", quotedBool((flags & kAccStatic) != 0));
    fprintf(gOutFile, " final=%s\n", quotedBool((flags & kAccFinal) != 0));
    // The "deprecated=" not knowable w/o parsing annotations.
    fprintf(gOutFile, " visibility=%s\n>\n", quotedVisibility(flags));

    // Parameters.
    if (typeDescriptor[0] != '(') {
      LOG(ERROR) << "ERROR: bad descriptor '" << typeDescriptor << "'";
      goto bail;
    }
    char* tmpBuf = reinterpret_cast<char*>(malloc(strlen(typeDescriptor) + 1));
    const char* base = typeDescriptor + 1;
    int argNum = 0;
    while (*base != ')') {
      char* cp = tmpBuf;
      while (*base == '[') {
        *cp++ = *base++;
      }
      if (*base == 'L') {
        // Copy through ';'.
        do {
          *cp = *base++;
        } while (*cp++ != ';');
      } else {
        // Primitive char, copy it.
        if (strchr("ZBCSIFJD", *base) == nullptr) {
          LOG(ERROR) << "ERROR: bad method signature '" << base << "'";
          break;  // while
        }
        *cp++ = *base++;
      }
      // Null terminate and display.
      *cp++ = '\0';
      std::unique_ptr<char[]> dot(descriptorToDot(tmpBuf));
      fprintf(gOutFile, "<parameter name=\"arg%d\" type=\"%s\">\n"
                        "</parameter>\n", argNum++, dot.get());
    }  // while
    free(tmpBuf);
    if (constructor) {
      fprintf(gOutFile, "</constructor>\n");
    } else {
      fprintf(gOutFile, "</method>\n");
    }
  }

bail:
  free(typeDescriptor);
  free(accessStr);
}

/*
 * Dumps a static or instance (class) field.
 */
static void dumpField(const ClassAccessor::Field& field, int i, const u1** data = nullptr) {
  // Bail for anything private if export only requested.
  const uint32_t flags = field.GetAccessFlags();
  if (gOptions.exportsOnly && (flags & (kAccPublic | kAccProtected)) == 0) {
    return;
  }

  const DexFile& dex_file = field.GetDexFile();
  const dex::FieldId& field_id = dex_file.GetFieldId(field.GetIndex());
  const char* name = dex_file.GetStringData(field_id.name_idx_);
  const char* typeDescriptor = dex_file.GetTypeDescriptor(field_id.type_idx_);
  const char* backDescriptor = dex_file.GetTypeDescriptor(field_id.class_idx_);
  char* accessStr = createAccessFlagStr(flags, AccessFor::kField);
  const uint32_t hiddenapiFlags = field.GetHiddenapiFlags();

  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "    #%d              : (in %s)\n", i, backDescriptor);
    fprintf(gOutFile, "      name          : '%s'\n", name);
    fprintf(gOutFile, "      type          : '%s'\n", typeDescriptor);
    fprintf(gOutFile, "      access        : 0x%04x (%s)\n", flags, accessStr);
    if (hiddenapiFlags != 0u) {
      fprintf(gOutFile,
              "      hiddenapi     : 0x%04x (%s)\n",
              hiddenapiFlags,
              GetHiddenapiFlagStr(hiddenapiFlags).c_str());
    }
    if (data != nullptr) {
      fputs("      value         : ", gOutFile);
      dumpEncodedValue(&dex_file, data);
      fputs("\n", gOutFile);
    }
  } else if (gOptions.outputFormat == OUTPUT_XML) {
    fprintf(gOutFile, "<field name=\"%s\"\n", name);
    std::unique_ptr<char[]> dot(descriptorToDot(typeDescriptor));
    fprintf(gOutFile, " type=\"%s\"\n", dot.get());
    fprintf(gOutFile, " transient=%s\n", quotedBool((flags & kAccTransient) != 0));
    fprintf(gOutFile, " volatile=%s\n", quotedBool((flags & kAccVolatile) != 0));
    // The "value=" is not knowable w/o parsing annotations.
    fprintf(gOutFile, " static=%s\n", quotedBool((flags & kAccStatic) != 0));
    fprintf(gOutFile, " final=%s\n", quotedBool((flags & kAccFinal) != 0));
    // The "deprecated=" is not knowable w/o parsing annotations.
    fprintf(gOutFile, " visibility=%s\n", quotedVisibility(flags));
    if (data != nullptr) {
      fputs(" value=\"", gOutFile);
      dumpEncodedValue(&dex_file, data);
      fputs("\"\n", gOutFile);
    }
    fputs(">\n</field>\n", gOutFile);
  }

  free(accessStr);
}

/*
 * Dumping a CFG.
 */
static void dumpCfg(const DexFile* dex_file, int idx) {
  ClassAccessor accessor(*dex_file, dex_file->GetClassDef(idx));
  for (const ClassAccessor::Method& method : accessor.GetMethods()) {
    if (method.GetCodeItem() != nullptr) {
      std::ostringstream oss;
      DumpMethodCFG(method, oss);
      fputs(oss.str().c_str(), gOutFile);
    }
  }
}

/*
 * Dumps the class.
 *
 * Note "idx" is a DexClassDef index, not a DexTypeId index.
 *
 * If "*pLastPackage" is nullptr or does not match the current class' package,
 * the value will be replaced with a newly-allocated string.
 */
static void dumpClass(const DexFile* pDexFile, int idx, char** pLastPackage) {
  const dex::ClassDef& pClassDef = pDexFile->GetClassDef(idx);

  // Omitting non-public class.
  if (gOptions.exportsOnly && (pClassDef.access_flags_ & kAccPublic) == 0) {
    return;
  }

  if (gOptions.showSectionHeaders) {
    dumpClassDef(pDexFile, idx);
  }

  if (gOptions.showAnnotations) {
    dumpClassAnnotations(pDexFile, idx);
  }

  if (gOptions.showCfg) {
    dumpCfg(pDexFile, idx);
    return;
  }

  // For the XML output, show the package name.  Ideally we'd gather
  // up the classes, sort them, and dump them alphabetically so the
  // package name wouldn't jump around, but that's not a great plan
  // for something that needs to run on the device.
  const char* classDescriptor = pDexFile->GetTypeDescriptor(pClassDef.class_idx_);
  if (!(classDescriptor[0] == 'L' &&
        classDescriptor[strlen(classDescriptor)-1] == ';')) {
    // Arrays and primitives should not be defined explicitly. Keep going?
    LOG(WARNING) << "Malformed class name '" << classDescriptor << "'";
  } else if (gOptions.outputFormat == OUTPUT_XML) {
    char* mangle = strdup(classDescriptor + 1);
    mangle[strlen(mangle)-1] = '\0';

    // Reduce to just the package name.
    char* lastSlash = strrchr(mangle, '/');
    if (lastSlash != nullptr) {
      *lastSlash = '\0';
    } else {
      *mangle = '\0';
    }

    for (char* cp = mangle; *cp != '\0'; cp++) {
      if (*cp == '/') {
        *cp = '.';
      }
    }  // for

    if (*pLastPackage == nullptr || strcmp(mangle, *pLastPackage) != 0) {
      // Start of a new package.
      if (*pLastPackage != nullptr) {
        fprintf(gOutFile, "</package>\n");
      }
      fprintf(gOutFile, "<package name=\"%s\"\n>\n", mangle);
      free(*pLastPackage);
      *pLastPackage = mangle;
    } else {
      free(mangle);
    }
  }

  // General class information.
  char* accessStr = createAccessFlagStr(pClassDef.access_flags_, AccessFor::kClass);
  const char* superclassDescriptor;
  if (!pClassDef.superclass_idx_.IsValid()) {
    superclassDescriptor = nullptr;
  } else {
    superclassDescriptor = pDexFile->GetTypeDescriptor(pClassDef.superclass_idx_);
  }
  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "Class #%d            -\n", idx);
    fprintf(gOutFile, "  Class descriptor  : '%s'\n", classDescriptor);
    fprintf(gOutFile, "  Access flags      : 0x%04x (%s)\n", pClassDef.access_flags_, accessStr);
    if (superclassDescriptor != nullptr) {
      fprintf(gOutFile, "  Superclass        : '%s'\n", superclassDescriptor);
    }
    fprintf(gOutFile, "  Interfaces        -\n");
  } else {
    std::unique_ptr<char[]> dot(descriptorClassToName(classDescriptor));
    fprintf(gOutFile, "<class name=\"%s\"\n", dot.get());
    if (superclassDescriptor != nullptr) {
      dot = descriptorToDot(superclassDescriptor);
      fprintf(gOutFile, " extends=\"%s\"\n", dot.get());
    }
    fprintf(gOutFile, " interface=%s\n",
            quotedBool((pClassDef.access_flags_ & kAccInterface) != 0));
    fprintf(gOutFile, " abstract=%s\n", quotedBool((pClassDef.access_flags_ & kAccAbstract) != 0));
    fprintf(gOutFile, " static=%s\n", quotedBool((pClassDef.access_flags_ & kAccStatic) != 0));
    fprintf(gOutFile, " final=%s\n", quotedBool((pClassDef.access_flags_ & kAccFinal) != 0));
    // The "deprecated=" not knowable w/o parsing annotations.
    fprintf(gOutFile, " visibility=%s\n", quotedVisibility(pClassDef.access_flags_));
    fprintf(gOutFile, ">\n");
  }

  // Interfaces.
  const dex::TypeList* pInterfaces = pDexFile->GetInterfacesList(pClassDef);
  if (pInterfaces != nullptr) {
    for (u4 i = 0; i < pInterfaces->Size(); i++) {
      dumpInterface(pDexFile, pInterfaces->GetTypeItem(i), i);
    }  // for
  }

  // Fields and methods.
  ClassAccessor accessor(*pDexFile, pClassDef, /* parse_hiddenapi_class_data= */ true);

  // Prepare data for static fields.
  const u1* sData = pDexFile->GetEncodedStaticFieldValuesArray(pClassDef);
  const u4 sSize = sData != nullptr ? DecodeUnsignedLeb128(&sData) : 0;

  // Static fields.
  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "  Static fields     -\n");
  }
  uint32_t i = 0u;
  for (const ClassAccessor::Field& field : accessor.GetStaticFields()) {
    dumpField(field, i, i < sSize ? &sData : nullptr);
    ++i;
  }

  // Instance fields.
  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "  Instance fields   -\n");
  }
  i = 0u;
  for (const ClassAccessor::Field& field : accessor.GetInstanceFields()) {
    dumpField(field, i);
    ++i;
  }

  // Direct methods.
  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "  Direct methods    -\n");
  }
  i = 0u;
  for (const ClassAccessor::Method& method : accessor.GetDirectMethods()) {
    dumpMethod(method, i);
    ++i;
  }

  // Virtual methods.
  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "  Virtual methods   -\n");
  }
  i = 0u;
  for (const ClassAccessor::Method& method : accessor.GetVirtualMethods()) {
    dumpMethod(method, i);
    ++i;
  }

  // End of class.
  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    const char* fileName;
    if (pClassDef.source_file_idx_.IsValid()) {
      fileName = pDexFile->GetStringData(pClassDef.source_file_idx_);
    } else {
      fileName = "unknown";
    }
    fprintf(gOutFile, "  source_file_idx   : %d (%s)\n\n",
            pClassDef.source_file_idx_.index_, fileName);
  } else if (gOptions.outputFormat == OUTPUT_XML) {
    fprintf(gOutFile, "</class>\n");
  }

  free(accessStr);
}

static void dumpMethodHandle(const DexFile* pDexFile, u4 idx) {
  const dex::MethodHandleItem& mh = pDexFile->GetMethodHandle(idx);
  const char* type = nullptr;
  bool is_instance = false;
  bool is_invoke = false;
  switch (static_cast<DexFile::MethodHandleType>(mh.method_handle_type_)) {
    case DexFile::MethodHandleType::kStaticPut:
      type = "put-static";
      is_instance = false;
      is_invoke = false;
      break;
    case DexFile::MethodHandleType::kStaticGet:
      type = "get-static";
      is_instance = false;
      is_invoke = false;
      break;
    case DexFile::MethodHandleType::kInstancePut:
      type = "put-instance";
      is_instance = true;
      is_invoke = false;
      break;
    case DexFile::MethodHandleType::kInstanceGet:
      type = "get-instance";
      is_instance = true;
      is_invoke = false;
      break;
    case DexFile::MethodHandleType::kInvokeStatic:
      type = "invoke-static";
      is_instance = false;
      is_invoke = true;
      break;
    case DexFile::MethodHandleType::kInvokeInstance:
      type = "invoke-instance";
      is_instance = true;
      is_invoke = true;
      break;
    case DexFile::MethodHandleType::kInvokeConstructor:
      type = "invoke-constructor";
      is_instance = true;
      is_invoke = true;
      break;
    case DexFile::MethodHandleType::kInvokeDirect:
      type = "invoke-direct";
      is_instance = true;
      is_invoke = true;
      break;
    case DexFile::MethodHandleType::kInvokeInterface:
      type = "invoke-interface";
      is_instance = true;
      is_invoke = true;
      break;
  }

  const char* declaring_class;
  const char* member;
  std::string member_type;
  if (type != nullptr) {
    if (is_invoke) {
      const dex::MethodId& method_id = pDexFile->GetMethodId(mh.field_or_method_idx_);
      declaring_class = pDexFile->GetMethodDeclaringClassDescriptor(method_id);
      member = pDexFile->GetMethodName(method_id);
      member_type = pDexFile->GetMethodSignature(method_id).ToString();
    } else {
      const dex::FieldId& field_id = pDexFile->GetFieldId(mh.field_or_method_idx_);
      declaring_class = pDexFile->GetFieldDeclaringClassDescriptor(field_id);
      member = pDexFile->GetFieldName(field_id);
      member_type = pDexFile->GetFieldTypeDescriptor(field_id);
    }
    if (is_instance) {
      member_type = android::base::StringPrintf("(%s%s", declaring_class, member_type.c_str() + 1);
    }
  } else {
    type = "?";
    declaring_class = "?";
    member = "?";
    member_type = "?";
  }

  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "Method handle #%u:\n", idx);
    fprintf(gOutFile, "  type        : %s\n", type);
    fprintf(gOutFile, "  target      : %s %s\n", declaring_class, member);
    fprintf(gOutFile, "  target_type : %s\n", member_type.c_str());
  }
}

static void dumpCallSite(const DexFile* pDexFile, u4 idx) {
  const dex::CallSiteIdItem& call_site_id = pDexFile->GetCallSiteId(idx);
  CallSiteArrayValueIterator it(*pDexFile, call_site_id);
  if (it.Size() < 3) {
    LOG(ERROR) << "ERROR: Call site " << idx << " has too few values.";
    return;
  }

  uint32_t method_handle_idx = static_cast<uint32_t>(it.GetJavaValue().i);
  it.Next();
  dex::StringIndex method_name_idx = static_cast<dex::StringIndex>(it.GetJavaValue().i);
  const char* method_name = pDexFile->GetStringData(method_name_idx);
  it.Next();
  dex::ProtoIndex method_type_idx = static_cast<dex::ProtoIndex>(it.GetJavaValue().i);
  const dex::ProtoId& method_type_id = pDexFile->GetProtoId(method_type_idx);
  std::string method_type = pDexFile->GetProtoSignature(method_type_id).ToString();
  it.Next();

  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "Call site #%u: // offset %u\n", idx, call_site_id.data_off_);
    fprintf(gOutFile, "  link_argument[0] : %u (MethodHandle)\n", method_handle_idx);
    fprintf(gOutFile, "  link_argument[1] : %s (String)\n", method_name);
    fprintf(gOutFile, "  link_argument[2] : %s (MethodType)\n", method_type.c_str());
  }

  size_t argument = 3;
  while (it.HasNext()) {
    const char* type;
    std::string value;
    switch (it.GetValueType()) {
      case EncodedArrayValueIterator::ValueType::kByte:
        type = "byte";
        value = android::base::StringPrintf("%u", it.GetJavaValue().b);
        break;
      case EncodedArrayValueIterator::ValueType::kShort:
        type = "short";
        value = android::base::StringPrintf("%d", it.GetJavaValue().s);
        break;
      case EncodedArrayValueIterator::ValueType::kChar:
        type = "char";
        value = android::base::StringPrintf("%u", it.GetJavaValue().c);
        break;
      case EncodedArrayValueIterator::ValueType::kInt:
        type = "int";
        value = android::base::StringPrintf("%d", it.GetJavaValue().i);
        break;
      case EncodedArrayValueIterator::ValueType::kLong:
        type = "long";
        value = android::base::StringPrintf("%" PRId64, it.GetJavaValue().j);
        break;
      case EncodedArrayValueIterator::ValueType::kFloat:
        type = "float";
        value = android::base::StringPrintf("%g", it.GetJavaValue().f);
        break;
      case EncodedArrayValueIterator::ValueType::kDouble:
        type = "double";
        value = android::base::StringPrintf("%g", it.GetJavaValue().d);
        break;
      case EncodedArrayValueIterator::ValueType::kMethodType: {
        type = "MethodType";
        dex::ProtoIndex proto_idx = static_cast<dex::ProtoIndex>(it.GetJavaValue().i);
        const dex::ProtoId& proto_id = pDexFile->GetProtoId(proto_idx);
        value = pDexFile->GetProtoSignature(proto_id).ToString();
        break;
      }
      case EncodedArrayValueIterator::ValueType::kMethodHandle:
        type = "MethodHandle";
        value = android::base::StringPrintf("%d", it.GetJavaValue().i);
        break;
      case EncodedArrayValueIterator::ValueType::kString: {
        type = "String";
        dex::StringIndex string_idx = static_cast<dex::StringIndex>(it.GetJavaValue().i);
        value = pDexFile->GetStringData(string_idx);
        break;
      }
      case EncodedArrayValueIterator::ValueType::kType: {
        type = "Class";
        dex::TypeIndex type_idx = static_cast<dex::TypeIndex>(it.GetJavaValue().i);
        const dex::TypeId& type_id = pDexFile->GetTypeId(type_idx);
        value = pDexFile->GetTypeDescriptor(type_id);
        break;
      }
      case EncodedArrayValueIterator::ValueType::kField:
      case EncodedArrayValueIterator::ValueType::kMethod:
      case EncodedArrayValueIterator::ValueType::kEnum:
      case EncodedArrayValueIterator::ValueType::kArray:
      case EncodedArrayValueIterator::ValueType::kAnnotation:
        // Unreachable based on current EncodedArrayValueIterator::Next().
        UNIMPLEMENTED(FATAL) << " type " << it.GetValueType();
        UNREACHABLE();
      case EncodedArrayValueIterator::ValueType::kNull:
        type = "Null";
        value = "null";
        break;
      case EncodedArrayValueIterator::ValueType::kBoolean:
        type = "boolean";
        value = it.GetJavaValue().z ? "true" : "false";
        break;
      case EncodedArrayValueIterator::ValueType::kEndOfInput:
        LOG(FATAL) << "Unreachable";
        UNREACHABLE();
    }

    if (gOptions.outputFormat == OUTPUT_PLAIN) {
      fprintf(gOutFile, "  link_argument[%zu] : %s (%s)\n", argument, value.c_str(), type);
    }

    it.Next();
    argument++;
  }
}

/*
 * Used to decide if we want to print or skip a string from string_ids
 */
static int isPrintable(const char* s) {
  for (size_t i = 0; i < strlen(s); i++) {
    if (!isprint((s[i]))) {
      return false;
    }
  }
  return true;
}

/*
 * Show all printable string in the string_ids section
 */
static void dumpStrings(const DexFile* pDexFile) {
  const DexFile::Header& pHeader = pDexFile->GetHeader();
  fprintf(gOutFile, "\nDisplaying %u strings from string_ids:\n", pHeader.string_ids_size_);

  for (uint32_t i = 0; i < pHeader.string_ids_size_; i++) {
    dex::StringIndex idx = static_cast<dex::StringIndex>(i);
    const char* string = pDexFile->GetStringData(idx);
    if (!isPrintable(string)) {
      string = "skipped (not printable)";
    }
    fprintf(gOutFile, "  string[%06u] - '%s'\n", i, string);
  }

  fprintf(gOutFile, "\n");
}

/*
 * Dumps the requested sections of the file.
 */
static void processDexFile(const char* fileName,
                           const DexFile* pDexFile, size_t i, size_t n) {
  if (gOptions.verbose) {
    fputs("Opened '", gOutFile);
    fputs(fileName, gOutFile);
    if (n > 1) {
      fprintf(gOutFile, ":%s", DexFileLoader::GetMultiDexClassesDexName(i).c_str());
    }
    fprintf(gOutFile, "', DEX version '%.3s'\n", pDexFile->GetHeader().magic_.data() + 4);
  }

  // Headers.
  if (gOptions.showFileHeaders) {
    dumpFileHeader(pDexFile);
  }

  // Strings.
  if (gOptions.showAllStrings) {
    dumpStrings(pDexFile);
  }

  // Iterate over all classes.
  char* package = nullptr;
  const u4 classDefsSize = pDexFile->GetHeader().class_defs_size_;
  for (u4 j = 0; j < classDefsSize; j++) {
    dumpClass(pDexFile, j, &package);
  }  // for

  // Iterate over all method handles.
  for (u4 j = 0; j < pDexFile->NumMethodHandles(); ++j) {
    dumpMethodHandle(pDexFile, j);
  }  // for

  // Iterate over all call site ids.
  for (u4 j = 0; j < pDexFile->NumCallSiteIds(); ++j) {
    dumpCallSite(pDexFile, j);
  }  // for

  // Free the last package allocated.
  if (package != nullptr) {
    fprintf(gOutFile, "</package>\n");
    free(package);
  }
}

/*
 * Processes a single file (either direct .dex or indirect .zip/.jar/.apk).
 */
int processFile(const char* fileName) {
  if (gOptions.verbose) {
    fprintf(gOutFile, "Processing '%s'...\n", fileName);
  }

  const bool kVerifyChecksum = !gOptions.ignoreBadChecksum;
  const bool kVerify = !gOptions.disableVerifier;
  std::string content;
  // If the file is not a .dex file, the function tries .zip/.jar/.apk files,
  // all of which are Zip archives with "classes.dex" inside.
  // TODO: add an api to android::base to read a std::vector<uint8_t>.
  if (!android::base::ReadFileToString(fileName, &content)) {
    LOG(ERROR) << "ReadFileToString failed";
    return -1;
  }
  DexFileLoaderErrorCode error_code;
  std::string error_msg;
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  DexFileLoader dex_file_loader(
      reinterpret_cast<const uint8_t*>(content.data()), content.size(), fileName);
  if (!dex_file_loader.Open(kVerify, kVerifyChecksum, &error_code, &error_msg, &dex_files)) {
    // Display returned error message to user. Note that this error behavior
    // differs from the error messages shown by the original Dalvik dexdump.
    LOG(ERROR) << error_msg;
    return -1;
  }

  // Success. Either report checksum verification or process
  // all dex files found in given file.
  if (gOptions.checksumOnly) {
    fprintf(gOutFile, "Checksum verified\n");
  } else {
    // Open XML context.
    if (gOptions.outputFormat == OUTPUT_XML) {
      fprintf(gOutFile, "<api>\n");
    }

    for (size_t i = 0, n = dex_files.size(); i < n; i++) {
      processDexFile(fileName, dex_files[i].get(), i, n);
    }

    // Close XML context.
    if (gOptions.outputFormat == OUTPUT_XML) {
      fprintf(gOutFile, "</api>\n");
    }
  }
  return 0;
}

}  // namespace art
