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
 * Implementation file of the dexlist utility.
 *
 * This is a re-implementation of the original dexlist utility that was
 * based on Dalvik functions in libdex into a new dexlist that is now
 * based on Art functions in libart instead. The output is identical to
 * the original for correct DEX files. Error messages may differ, however.
 *
 * List all methods in all concrete classes in one or more DEX files.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <android-base/file.h>
#include <android-base/logging.h>

#include "base/mem_map.h"
#include "dex/class_accessor-inl.h"
#include "dex/code_item_accessors-inl.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_loader.h"

namespace art {

static const char* gProgName = "dexlist";

/* Command-line options. */
static struct {
  char* argCopy;
  const char* classToFind;
  const char* methodToFind;
  const char* outputFileName;
} gOptions;

/*
 * Output file. Defaults to stdout.
 */
static FILE* gOutFile = stdout;

/*
 * Data types that match the definitions in the VM specification.
 */
using u1 = uint8_t;
using u4 = uint32_t;
using u8 = uint64_t;

/*
 * Returns a newly-allocated string for the "dot version" of the class
 * name for the given type descriptor. That is, The initial "L" and
 * final ";" (if any) have been removed and all occurrences of '/'
 * have been changed to '.'.
 */
static std::unique_ptr<char[]> descriptorToDot(const char* str) {
  size_t len = strlen(str);
  if (str[0] == 'L') {
    len -= 2;  // Two fewer chars to copy (trims L and ;).
    str++;     // Start past 'L'.
  }
  std::unique_ptr<char[]> newStr(new char[len + 1]);
  for (size_t i = 0; i < len; i++) {
    newStr[i] = (str[i] == '/') ? '.' : str[i];
  }
  newStr[len] = '\0';
  return newStr;
}

/*
 * Dumps a method.
 */
static void dumpMethod(const DexFile* pDexFile,
                       const char* fileName,
                       u4 idx,
                       [[maybe_unused]] u4 flags,
                       const dex::CodeItem* pCode,
                       u4 codeOffset) {
  // Abstract and native methods don't get listed.
  if (pCode == nullptr || codeOffset == 0) {
    return;
  }
  CodeItemDebugInfoAccessor accessor(*pDexFile, pCode, idx);

  // Method information.
  const dex::MethodId& pMethodId = pDexFile->GetMethodId(idx);
  const char* methodName = pDexFile->GetStringData(pMethodId.name_idx_);
  const char* classDescriptor = pDexFile->GetTypeDescriptor(pMethodId.class_idx_);
  std::unique_ptr<char[]> className(descriptorToDot(classDescriptor));
  const u4 insnsOff = codeOffset + 0x10;

  // Don't list methods that do not match a particular query.
  if (gOptions.methodToFind != nullptr &&
      (strcmp(gOptions.classToFind, className.get()) != 0 ||
       strcmp(gOptions.methodToFind, methodName) != 0)) {
    return;
  }

  // If the filename is empty, then set it to something printable.
  if (fileName == nullptr || fileName[0] == 0) {
    fileName = "(none)";
  }

  // We just want to catch the number of the first line in the method, which *should* correspond to
  // the first entry from the table.
  int first_line = -1;
  accessor.DecodeDebugPositionInfo([&](const DexFile::PositionInfo& entry) {
    first_line = entry.line_;
    return true;  // Early exit since we only want the first line.
  });

  // Method signature.
  const Signature signature = pDexFile->GetMethodSignature(pMethodId);
  char* typeDesc = strdup(signature.ToString().c_str());

  // Dump actual method information.
  fprintf(gOutFile, "0x%08x %d %s %s %s %s %d\n",
          insnsOff, accessor.InsnsSizeInCodeUnits() * 2,
          className.get(), methodName, typeDesc, fileName, first_line);

  free(typeDesc);
}

/*
 * Runs through all direct and virtual methods in the class.
 */
void dumpClass(const DexFile* pDexFile, u4 idx) {
  const dex::ClassDef& class_def = pDexFile->GetClassDef(idx);

  const char* fileName = nullptr;
  if (class_def.source_file_idx_.IsValid()) {
    fileName = pDexFile->GetStringData(class_def.source_file_idx_);
  }

  ClassAccessor accessor(*pDexFile, class_def);
  for (const ClassAccessor::Method& method : accessor.GetMethods()) {
    dumpMethod(pDexFile,
               fileName,
               method.GetIndex(),
               method.GetAccessFlags(),
               method.GetCodeItem(),
               method.GetCodeItemOffset());
  }
}

/*
 * Processes a single file (either direct .dex or indirect .zip/.jar/.apk).
 */
static int processFile(const char* fileName) {
  // If the file is not a .dex file, the function tries .zip/.jar/.apk files,
  // all of which are Zip archives with "classes.dex" inside.
  static constexpr bool kVerifyChecksum = true;
  std::string content;
  // TODO: add an api to android::base to read a std::vector<uint8_t>.
  if (!android::base::ReadFileToString(fileName, &content)) {
    LOG(ERROR) << "ReadFileToString failed";
    return -1;
  }
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  DexFileLoaderErrorCode error_code;
  std::string error_msg;
  DexFileLoader dex_file_loader(
      reinterpret_cast<const uint8_t*>(content.data()), content.size(), fileName);
  if (!dex_file_loader.Open(
          /*verify=*/true, kVerifyChecksum, &error_code, &error_msg, &dex_files)) {
    LOG(ERROR) << error_msg;
    return -1;
  }

  // Success. Iterate over all dex files found in given file.
  fprintf(gOutFile, "#%s\n", fileName);
  for (size_t i = 0; i < dex_files.size(); i++) {
    // Iterate over all classes in one dex file.
    const DexFile* pDexFile = dex_files[i].get();
    const u4 classDefsSize = pDexFile->GetHeader().class_defs_size_;
    for (u4 idx = 0; idx < classDefsSize; idx++) {
      dumpClass(pDexFile, idx);
    }
  }
  return 0;
}

/*
 * Shows usage.
 */
static void usage() {
  LOG(ERROR) << "Copyright (C) 2007 The Android Open Source Project\n";
  LOG(ERROR) << gProgName << ": [-m p.c.m] [-o outfile] dexfile...";
  LOG(ERROR) << "";
}

/*
 * Main driver of the dexlist utility.
 */
int dexlistDriver(int argc, char** argv) {
  // Reset options.
  bool wantUsage = false;
  memset(&gOptions, 0, sizeof(gOptions));

  // Parse all arguments.
  while (true) {
    const int ic = getopt(argc, argv, "o:m:");
    if (ic < 0) {
      break;  // done
    }
    switch (ic) {
      case 'o':  // output file
        gOptions.outputFileName = optarg;
        break;
      case 'm':
        // If -m p.c.m is given, then find all instances of the
        // fully-qualified method name. This isn't really what
        // dexlist is for, but it's easy to do it here.
        {
          gOptions.argCopy = strdup(optarg);
          char* meth = strrchr(gOptions.argCopy, '.');
          if (meth == nullptr) {
            LOG(ERROR) << "Expected: package.Class.method";
            wantUsage = true;
          } else {
            *meth = '\0';
            gOptions.classToFind = gOptions.argCopy;
            gOptions.methodToFind = meth + 1;
          }
        }
        break;
      default:
        wantUsage = true;
        break;
    }  // switch
  }  // while

  // Detect early problems.
  if (optind == argc) {
    LOG(ERROR) << "No file specified";
    wantUsage = true;
  }
  if (wantUsage) {
    usage();
    free(gOptions.argCopy);
    return 2;
  }

  // Open alternative output file.
  if (gOptions.outputFileName) {
    gOutFile = fopen(gOptions.outputFileName, "we");
    if (!gOutFile) {
      PLOG(ERROR) << "Can't open " << gOptions.outputFileName;
      free(gOptions.argCopy);
      return 1;
    }
  }

  // Process all files supplied on command line. If one of them fails we
  // continue on, only returning a failure at the end.
  int result = 0;
  while (optind < argc) {
    result |= processFile(argv[optind++]);
  }  // while

  free(gOptions.argCopy);
  return result != 0 ? 1 : 0;
}

}  // namespace art

int main(int argc, char** argv) {
  // Output all logging to stderr.
  android::base::SetLogger(android::base::StderrLogger);
  art::MemMap::Init();

  return art::dexlistDriver(argc, argv);
}

