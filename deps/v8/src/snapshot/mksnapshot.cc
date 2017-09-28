// Copyright 2006-2008 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <signal.h>
#include <stdio.h>

#include "include/libplatform/libplatform.h"
#include "src/assembler.h"
#include "src/base/platform/platform.h"
#include "src/flags.h"
#include "src/msan.h"
#include "src/snapshot/natives.h"
#include "src/snapshot/partial-serializer.h"
#include "src/snapshot/startup-serializer.h"

class SnapshotWriter {
 public:
  SnapshotWriter() : snapshot_cpp_path_(NULL), snapshot_blob_path_(NULL) {}

  void SetSnapshotFile(const char* snapshot_cpp_file) {
    snapshot_cpp_path_ = snapshot_cpp_file;
  }

  void SetStartupBlobFile(const char* snapshot_blob_file) {
    snapshot_blob_path_ = snapshot_blob_file;
  }

  void WriteSnapshot(v8::StartupData blob) const {
    // TODO(crbug/633159): if we crash before the files have been fully created,
    // we end up with a corrupted snapshot file. The build step would succeed,
    // but the build target is unusable. Ideally we would write out temporary
    // files and only move them to the final destination as last step.
    i::Vector<const i::byte> blob_vector(
        reinterpret_cast<const i::byte*>(blob.data), blob.raw_size);
    MaybeWriteSnapshotFile(blob_vector);
    MaybeWriteStartupBlob(blob_vector);
  }

 private:
  void MaybeWriteStartupBlob(const i::Vector<const i::byte>& blob) const {
    if (!snapshot_blob_path_) return;

    FILE* fp = GetFileDescriptorOrDie(snapshot_blob_path_);
    size_t written = fwrite(blob.begin(), 1, blob.length(), fp);
    fclose(fp);
    if (written != static_cast<size_t>(blob.length())) {
      i::PrintF("Writing snapshot file failed.. Aborting.\n");
      remove(snapshot_blob_path_);
      exit(1);
    }
  }

  void MaybeWriteSnapshotFile(const i::Vector<const i::byte>& blob) const {
    if (!snapshot_cpp_path_) return;

    FILE* fp = GetFileDescriptorOrDie(snapshot_cpp_path_);

    WriteFilePrefix(fp);
    WriteData(fp, blob);
    WriteFileSuffix(fp);

    fclose(fp);
  }

  static void WriteFilePrefix(FILE* fp) {
    fprintf(fp, "// Autogenerated snapshot file. Do not edit.\n\n");
    fprintf(fp, "#include \"src/v8.h\"\n");
    fprintf(fp, "#include \"src/base/platform/platform.h\"\n\n");
    fprintf(fp, "#include \"src/snapshot/snapshot.h\"\n\n");
    fprintf(fp, "namespace v8 {\n");
    fprintf(fp, "namespace internal {\n\n");
  }

  static void WriteFileSuffix(FILE* fp) {
    fprintf(fp, "const v8::StartupData* Snapshot::DefaultSnapshotBlob() {\n");
    fprintf(fp, "  return &blob;\n");
    fprintf(fp, "}\n\n");
    fprintf(fp, "}  // namespace internal\n");
    fprintf(fp, "}  // namespace v8\n");
  }

  static void WriteData(FILE* fp, const i::Vector<const i::byte>& blob) {
    fprintf(fp, "static const byte blob_data[] = {\n");
    WriteSnapshotData(fp, blob);
    fprintf(fp, "};\n");
    fprintf(fp, "static const int blob_size = %d;\n", blob.length());
    fprintf(fp, "static const v8::StartupData blob =\n");
    fprintf(fp, "{ (const char*) blob_data, blob_size };\n");
  }

  static void WriteSnapshotData(FILE* fp,
                                const i::Vector<const i::byte>& blob) {
    for (int i = 0; i < blob.length(); i++) {
      if ((i & 0x1f) == 0x1f) fprintf(fp, "\n");
      if (i > 0) fprintf(fp, ",");
      fprintf(fp, "%u", static_cast<unsigned char>(blob.at(i)));
    }
    fprintf(fp, "\n");
  }

  static FILE* GetFileDescriptorOrDie(const char* filename) {
    FILE* fp = v8::base::OS::FOpen(filename, "wb");
    if (fp == NULL) {
      i::PrintF("Unable to open file \"%s\" for writing.\n", filename);
      exit(1);
    }
    return fp;
  }

  const char* snapshot_cpp_path_;
  const char* snapshot_blob_path_;
};

char* GetExtraCode(char* filename, const char* description) {
  if (filename == NULL || strlen(filename) == 0) return NULL;
  ::printf("Loading script for %s: %s\n", description, filename);
  FILE* file = v8::base::OS::FOpen(filename, "rb");
  if (file == NULL) {
    fprintf(stderr, "Failed to open '%s': errno %d\n", filename, errno);
    exit(1);
  }
  fseek(file, 0, SEEK_END);
  size_t size = ftell(file);
  rewind(file);
  char* chars = new char[size + 1];
  chars[size] = '\0';
  for (size_t i = 0; i < size;) {
    size_t read = fread(&chars[i], 1, size - i, file);
    if (ferror(file)) {
      fprintf(stderr, "Failed to read '%s': errno %d\n", filename, errno);
      exit(1);
    }
    i += read;
  }
  fclose(file);
  return chars;
}


int main(int argc, char** argv) {
  // Make mksnapshot runs predictable to create reproducible snapshots.
  i::FLAG_predictable = true;

  // Print the usage if an error occurs when parsing the command line
  // flags or if the help flag is set.
  int result = i::FlagList::SetFlagsFromCommandLine(&argc, argv, true);
  if (result > 0 || (argc > 3) || i::FLAG_help) {
    ::printf("Usage: %s --startup_src=... --startup_blob=... [extras]\n",
             argv[0]);
    i::FlagList::PrintHelp();
    return !i::FLAG_help;
  }

  i::CpuFeatures::Probe(true);
  v8::V8::InitializeICUDefaultLocation(argv[0]);
  v8::Platform* platform = v8::platform::CreateDefaultPlatform();
  v8::V8::InitializePlatform(platform);
  v8::V8::Initialize();

  {
    SnapshotWriter writer;
    if (i::FLAG_startup_src) writer.SetSnapshotFile(i::FLAG_startup_src);
    if (i::FLAG_startup_blob) writer.SetStartupBlobFile(i::FLAG_startup_blob);

    char* embed_script = GetExtraCode(argc >= 2 ? argv[1] : NULL, "embedding");
    v8::StartupData blob = v8::V8::CreateSnapshotDataBlob(embed_script);
    delete[] embed_script;

    char* warmup_script = GetExtraCode(argc >= 3 ? argv[2] : NULL, "warm up");
    if (warmup_script) {
      v8::StartupData cold = blob;
      blob = v8::V8::WarmUpSnapshotDataBlob(cold, warmup_script);
      delete[] cold.data;
      delete[] warmup_script;
    }

    CHECK(blob.data);
    writer.WriteSnapshot(blob);
    delete[] blob.data;
  }

  v8::V8::Dispose();
  v8::V8::ShutdownPlatform();
  delete platform;
  return 0;
}
