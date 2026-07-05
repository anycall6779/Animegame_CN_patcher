#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdarg>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" const uint8_t animegame_translation_blob[];
extern "C" const uint8_t *animegame_translation_blob_end;

namespace {

constexpr char kTag[] = "ANIMEGAME_LOCALIFY";
constexpr char kLogPrefix[] = "[ANIMEGAME_ANDROID]";
constexpr uint8_t kBlobMagic[8] = {'H', 'S', 'O', 'N', 'T', 'R', '1', 0};

struct Il2CppString {
  void *klass;
  void *monitor;
  int32_t length;
  uint16_t chars[0];
};

using Il2CppDomainGet = void *(*)();
using Il2CppThreadAttach = void *(*)(void *);
using Il2CppDomainAssemblyOpen = void *(*)(void *, const char *);
using Il2CppAssemblyGetImage = void *(*)(void *);
using Il2CppClassFromName = void *(*)(void *, const char *, const char *);
using Il2CppClassGetMethodFromName = void *(*)(void *, const char *, int);
using Il2CppStringNew = Il2CppString *(*)(const char *);
using Il2CppResolveIcall = void *(*)(const char *);
using GetTextFn = Il2CppString *(*)(void *, void *);
using ICallGetTextFn = Il2CppString *(*)(void *);

Il2CppDomainGet il2cpp_domain_get_fn = nullptr;
Il2CppThreadAttach il2cpp_thread_attach_fn = nullptr;
Il2CppDomainAssemblyOpen il2cpp_domain_assembly_open_fn = nullptr;
Il2CppAssemblyGetImage il2cpp_assembly_get_image_fn = nullptr;
Il2CppClassFromName il2cpp_class_from_name_fn = nullptr;
Il2CppClassGetMethodFromName il2cpp_class_get_method_from_name_fn = nullptr;
Il2CppStringNew il2cpp_string_new_fn = nullptr;
Il2CppResolveIcall il2cpp_resolve_icall_fn = nullptr;

GetTextFn original_get_text = nullptr;
ICallGetTextFn original_get_text_icall = nullptr;

std::once_flag translation_once;
std::unordered_map<std::string, std::string> translations;
std::mutex cache_mutex;
std::unordered_map<std::string, std::string> replacement_cache;
std::mutex apk_path_mutex;
std::string apk_path;
std::vector<uint8_t> external_translation_blob;

void logi(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  __android_log_vprint(ANDROID_LOG_INFO, kTag, fmt, args);
  va_end(args);
}

void loge(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  __android_log_vprint(ANDROID_LOG_ERROR, kTag, fmt, args);
  va_end(args);
}

template <typename T>
bool resolve_symbol(T &slot, const char *name) {
  void *symbol = dlsym(RTLD_DEFAULT, name);
  if (symbol == nullptr) {
    void *handle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_GLOBAL);
    if (handle != nullptr)
      symbol = dlsym(handle, name);
  }
  if (symbol == nullptr)
    return false;
  slot = reinterpret_cast<T>(symbol);
  return true;
}

bool resolve_il2cpp() {
  bool ok = true;
  ok &= resolve_symbol(il2cpp_domain_get_fn, "il2cpp_domain_get");
  ok &= resolve_symbol(il2cpp_thread_attach_fn, "il2cpp_thread_attach");
  ok &= resolve_symbol(il2cpp_domain_assembly_open_fn, "il2cpp_domain_assembly_open");
  ok &= resolve_symbol(il2cpp_assembly_get_image_fn, "il2cpp_assembly_get_image");
  ok &= resolve_symbol(il2cpp_class_from_name_fn, "il2cpp_class_from_name");
  ok &= resolve_symbol(il2cpp_class_get_method_from_name_fn, "il2cpp_class_get_method_from_name");
  ok &= resolve_symbol(il2cpp_string_new_fn, "il2cpp_string_new");
  resolve_symbol(il2cpp_resolve_icall_fn, "il2cpp_resolve_icall");
  return ok;
}

bool read_u32(const uint8_t *&cursor, const uint8_t *end, uint32_t &value) {
  if (cursor + 4 > end)
    return false;
  value = static_cast<uint32_t>(cursor[0]) |
      (static_cast<uint32_t>(cursor[1]) << 8) |
      (static_cast<uint32_t>(cursor[2]) << 16) |
      (static_cast<uint32_t>(cursor[3]) << 24);
  cursor += 4;
  return true;
}

uint16_t get_u16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t get_u32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) |
      (static_cast<uint32_t>(p[1]) << 8) |
      (static_cast<uint32_t>(p[2]) << 16) |
      (static_cast<uint32_t>(p[3]) << 24);
}

std::vector<uint8_t> read_file_bytes(const std::string &path) {
  std::vector<uint8_t> out;
  FILE *file = fopen(path.c_str(), "rb");
  if (file == nullptr)
    return out;
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (size > 0) {
    out.resize(static_cast<size_t>(size));
    if (fread(out.data(), 1, out.size(), file) != out.size())
      out.clear();
  }
  fclose(file);
  return out;
}

std::vector<uint8_t> read_stored_zip_entry(const std::string &zip_path, const char *entry_name) {
  std::vector<uint8_t> zip = read_file_bytes(zip_path);
  if (zip.size() < 22)
    return {};

  size_t eocd = std::string::npos;
  size_t min_pos = zip.size() > 0x10000 + 22 ? zip.size() - (0x10000 + 22) : 0;
  for (size_t pos = zip.size() - 22; pos + 4 <= zip.size() && pos >= min_pos; pos--) {
    if (get_u32(zip.data() + pos) == 0x06054b50) {
      eocd = pos;
      break;
    }
    if (pos == 0)
      break;
  }
  if (eocd == std::string::npos)
    return {};

  uint32_t cd_size = get_u32(zip.data() + eocd + 12);
  uint32_t cd_offset = get_u32(zip.data() + eocd + 16);
  if (cd_offset + cd_size > zip.size())
    return {};

  size_t cursor = cd_offset;
  size_t cd_end = cd_offset + cd_size;
  const size_t want_len = strlen(entry_name);
  while (cursor + 46 <= cd_end && get_u32(zip.data() + cursor) == 0x02014b50) {
    uint16_t method = get_u16(zip.data() + cursor + 10);
    uint32_t compressed_size = get_u32(zip.data() + cursor + 20);
    uint32_t size = get_u32(zip.data() + cursor + 24);
    uint16_t name_len = get_u16(zip.data() + cursor + 28);
    uint16_t extra_len = get_u16(zip.data() + cursor + 30);
    uint16_t comment_len = get_u16(zip.data() + cursor + 32);
    uint32_t local_offset = get_u32(zip.data() + cursor + 42);
    size_t name_pos = cursor + 46;
    if (name_pos + name_len > cd_end)
      return {};
    bool match = name_len == want_len && memcmp(zip.data() + name_pos, entry_name, want_len) == 0;
    if (match) {
      if (method != 0 || compressed_size != size)
        return {};
      if (local_offset + 30 > zip.size() || get_u32(zip.data() + local_offset) != 0x04034b50)
        return {};
      uint16_t local_name_len = get_u16(zip.data() + local_offset + 26);
      uint16_t local_extra_len = get_u16(zip.data() + local_offset + 28);
      size_t data_pos = local_offset + 30 + local_name_len + local_extra_len;
      if (data_pos + size > zip.size())
        return {};
      return std::vector<uint8_t>(zip.begin() + data_pos, zip.begin() + data_pos + size);
    }
    cursor = name_pos + name_len + extra_len + comment_len;
  }
  return {};
}

void load_external_translation_blob() {
  std::string path;
  {
    std::lock_guard<std::mutex> lock(apk_path_mutex);
    path = apk_path;
  }
  if (path.empty())
    return;
  std::vector<uint8_t> blob = read_stored_zip_entry(path, "assets/animegame_translation_blob.bin");
  if (blob.empty())
    return;
  if (blob.size() < 12 || memcmp(blob.data(), kBlobMagic, sizeof(kBlobMagic)) != 0) {
    loge("%s external translation blob is invalid", kLogPrefix);
    return;
  }
  external_translation_blob = std::move(blob);
  logi("%s loaded external translation blob bytes=%zu", kLogPrefix, external_translation_blob.size());
}

void load_translations_once() {
  load_external_translation_blob();
  const uint8_t *cursor = external_translation_blob.empty() ? animegame_translation_blob : external_translation_blob.data();
  const uint8_t *end = external_translation_blob.empty()
      ? animegame_translation_blob_end
      : external_translation_blob.data() + external_translation_blob.size();
  if (end - cursor < 12 || memcmp(cursor, kBlobMagic, sizeof(kBlobMagic)) != 0) {
    loge("%s invalid translation blob", kLogPrefix);
    return;
  }
  cursor += sizeof(kBlobMagic);

  uint32_t count = 0;
  if (!read_u32(cursor, end, count))
    return;

  translations.reserve(count);
  for (uint32_t i = 0; i != count; i++) {
    uint32_t key_len = 0;
    uint32_t value_len = 0;
    if (!read_u32(cursor, end, key_len) || !read_u32(cursor, end, value_len) ||
        cursor + key_len + value_len > end) {
      translations.clear();
      loge("%s truncated translation blob at entry %u", kLogPrefix, i);
      return;
    }
    std::string key(reinterpret_cast<const char *>(cursor), key_len);
    cursor += key_len;
    std::string value(reinterpret_cast<const char *>(cursor), value_len);
    cursor += value_len;
    translations.emplace(std::move(key), std::move(value));
  }
  logi("%s loaded %zu translations", kLogPrefix, translations.size());
}

void append_utf8(uint32_t cp, std::string &out) {
  if (cp <= 0x7f) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7ff) {
    out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else if (cp <= 0xffff) {
    out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
}

std::string il2cpp_string_to_utf8(Il2CppString *string) {
  std::string out;
  if (string == nullptr || string->length <= 0)
    return out;
  out.reserve(static_cast<size_t>(string->length) * 3);
  for (int32_t i = 0; i < string->length; i++) {
    uint32_t cp = string->chars[i];
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < string->length) {
      uint32_t low = string->chars[i + 1];
      if (low >= 0xdc00 && low <= 0xdfff) {
        cp = 0x10000 + (((cp - 0xd800) << 10) | (low - 0xdc00));
        i++;
      }
    }
    append_utf8(cp, out);
  }
  return out;
}

std::vector<std::string> split_crlf(const std::string &input) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (true) {
    size_t pos = input.find("\r\n", start);
    if (pos == std::string::npos) {
      lines.emplace_back(input.substr(start));
      return lines;
    }
    lines.emplace_back(input.substr(start, pos - start));
    start = pos + 2;
  }
}

std::string sanitize_table_field(const std::string &value);

bool build_replacement(const std::string &raw, std::string &replacement) {
  if (raw.rfind("TEXT_ID", 0) != 0)
    return false;

  std::call_once(translation_once, load_translations_once);
  if (translations.empty())
    return false;

  std::vector<std::string> lines = split_crlf(raw);
  if (lines.size() < 7)
    return false;

  const std::string &textmap_key = lines[6];
  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto cached = replacement_cache.find(textmap_key);
    if (cached != replacement_cache.end()) {
      replacement = cached->second;
      return true;
    }
  }

  replacement.clear();
  replacement.reserve(raw.size());
  replacement.append(lines[0]).append("\r\n")
      .append(lines[1]).append("\r\n")
      .append(lines[2]).append("\r\n")
      .append(lines[3]);

  size_t replaced = 0;
  for (size_t i = 4; i < lines.size(); i++) {
    const std::string &line = lines[i];
    size_t first_tab = line.find('\t');
    size_t second_tab = first_tab == std::string::npos ? std::string::npos : line.find('\t', first_tab + 1);
    if (first_tab != std::string::npos && second_tab != std::string::npos) {
      std::string id = line.substr(0, first_tab);
      auto translated = translations.find(id);
      if (translated != translations.end()) {
        std::string safe_translation = sanitize_table_field(translated->second);
        size_t third_tab = line.find('\t', second_tab + 1);
        std::string third = third_tab == std::string::npos
            ? line.substr(second_tab + 1)
            : line.substr(second_tab + 1, third_tab - second_tab - 1);
        replacement.append("\r\n").append(id).append("\t").append(safe_translation).append("\t").append(third);
        replaced++;
        continue;
      }
    }
    replacement.append("\r\n").append(line);
  }

  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    replacement_cache[textmap_key] = replacement;
  }
  logi("%s TEXT_ID asset key='%s' lines=%zu replaced=%zu raw=%zu output=%zu",
      kLogPrefix, textmap_key.c_str(), lines.size(), replaced, raw.size(), replacement.size());
  return true;
}

std::string sanitize_table_field(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); i++) {
    char ch = value[i];
    if (ch == '\r') {
      if (i + 1 < value.size() && value[i + 1] == '\n') {
        i++;
        out.append("#n");
      } else {
        out.append("#r");
      }
    } else if (ch == '\n') {
      out.append("#n");
    } else if (ch == '\t') {
      out.append("#t");
    } else if (ch != '\0') {
      out.push_back(ch);
    }
  }
  return out;
}

Il2CppString *process_text_result(Il2CppString *original, const char *source) {
  if (original == nullptr || il2cpp_string_new_fn == nullptr)
    return original;
  std::string raw = il2cpp_string_to_utf8(original);
  std::string replacement;
  if (!build_replacement(raw, replacement))
    return original;
  logi("%s %s replaced TEXT_ID text", kLogPrefix, source);
  return il2cpp_string_new_fn(replacement.c_str());
}

Il2CppString *hook_get_text(void *self, void *method_info) {
  Il2CppString *original = original_get_text != nullptr ? original_get_text(self, method_info) : nullptr;
  return process_text_result(original, "method-hook");
}

Il2CppString *hook_get_text_icall(void *self) {
  Il2CppString *original = original_get_text_icall != nullptr ? original_get_text_icall(self) : nullptr;
  return process_text_result(original, "icall-hook");
}

bool make_writable(void *address) {
  const size_t page_size = static_cast<size_t>(getpagesize());
  uintptr_t page = reinterpret_cast<uintptr_t>(address) & ~(static_cast<uintptr_t>(page_size) - 1);
  return mprotect(reinterpret_cast<void *>(page), page_size, PROT_READ | PROT_WRITE) == 0 ||
      mprotect(reinterpret_cast<void *>(page), page_size, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

bool should_scan_mapping(const char *perms, const char *path) {
  if (perms == nullptr || perms[0] != 'r' || perms[1] != 'w')
    return false;
  if (path != nullptr && strstr(path, "libanimegame_native_localify.so") != nullptr)
    return false;
  return true;
}

size_t patch_pointer_cache(void *original, void *replacement, const char *label) {
  FILE *maps = fopen("/proc/self/maps", "r");
  if (maps == nullptr)
    return 0;

  size_t patched = 0;
  char line[1024];
  while (fgets(line, sizeof(line), maps) != nullptr) {
    unsigned long long start = 0;
    unsigned long long end = 0;
    char perms[5] = {};
    char path[512] = {};
    int fields = sscanf(line, "%llx-%llx %4s %*s %*s %*s %511[^\n]", &start, &end, perms, path);
    if (fields < 3 || !should_scan_mapping(perms, fields >= 4 ? path : ""))
      continue;
    if (end <= start || end - start > 256ULL * 1024ULL * 1024ULL)
      continue;

    uintptr_t cursor = static_cast<uintptr_t>(start);
    uintptr_t limit = static_cast<uintptr_t>(end);
    for (; cursor + sizeof(void *) <= limit; cursor += sizeof(void *)) {
      void **slot = reinterpret_cast<void **>(cursor);
      if (*slot != original)
        continue;
      *slot = replacement;
      patched++;
      logi("%s icall-cache-hooked %s slot=%p original=%p replacement=%p",
          kLogPrefix, label, slot, original, replacement);
      if (patched >= 64) {
        fclose(maps);
        return patched;
      }
    }
  }

  fclose(maps);
  return patched;
}

bool install_icall_hook() {
  if (il2cpp_resolve_icall_fn == nullptr)
    return false;
  void *resolved = il2cpp_resolve_icall_fn("UnityEngine.TextAsset::get_text()");
  if (resolved == nullptr)
    resolved = il2cpp_resolve_icall_fn("UnityEngine.TextAsset::get_text");
  if (resolved == nullptr)
    return false;
  if (original_get_text_icall == nullptr)
    original_get_text_icall = reinterpret_cast<ICallGetTextFn>(resolved);

  size_t patched = patch_pointer_cache(
      resolved, reinterpret_cast<void *>(&hook_get_text_icall), "UnityEngine.TextAsset.get_text");
  if (patched == 0) {
    logi("%s resolved UnityEngine.TextAsset::get_text icall -> %p; cache not ready yet",
        kLogPrefix, resolved);
    return false;
  }
  return true;
}

bool install_hook() {
  if (!resolve_il2cpp())
    return false;
  return install_icall_hook();
}

void *installer_thread(void *) {
  logi("%s installer started", kLogPrefix);
  sleep(8);
  for (int attempt = 0; attempt < 300; attempt++) {
    if (install_hook()) {
      logi("%s hook installation complete attempt=%d", kLogPrefix, attempt);
      return nullptr;
    }
    usleep(500 * 1000);
  }
  loge("%s failed to install hook before timeout", kLogPrefix);
  return nullptr;
}

__attribute__((constructor))
void animegame_native_localify_init() {
  logi("%s native library loaded", kLogPrefix);
  pthread_t thread{};
  if (pthread_create(&thread, nullptr, installer_thread, nullptr) == 0)
    pthread_detach(thread);
}

extern "C" JNIEXPORT void JNICALL
Java_com_combosdk_openapi_ComboAppProxy_nativeSetApkPath(JNIEnv *env, jclass, jstring path) {
  const char *chars = env->GetStringUTFChars(path, nullptr);
  if (chars != nullptr) {
    {
      std::lock_guard<std::mutex> lock(apk_path_mutex);
      apk_path = chars;
    }
    logi("%s apk path set: %s", kLogPrefix, chars);
    env->ReleaseStringUTFChars(path, chars);
  }
}

}  // namespace
