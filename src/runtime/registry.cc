/*!
 *  Copyright (c) 2019 by Contributors if not otherwise specified
 * \file registry.cc
 * \brief The global registry of packed function.
 */
#include <dmlc/logging.h>
#include <dmlc/thread_local.h>
#include <decord/runtime/registry.h>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <array>
#include "runtime_base.h"

namespace decord {
namespace runtime {

struct Registry::Manager {
  // map storing the functions.
  // We delibrately used raw pointer
  // This is because PackedFunc can contain callbacks into the host languge(python)
  // and the resource can become invalid because of indeterminstic order of destruction.
  // The resources will only be recycled during program exit.
  std::unordered_map<std::string, Registry*> fmap;
  // vtable for extension type
  std::array<ExtTypeVTable, kExtEnd> ext_vtable;
  // mutex
  std::mutex mutex;

  Manager() {
    for (auto& x : ext_vtable) {
      x.destroy = nullptr;
    }
  }

  static Manager* Global() {
    // We deliberately leak the Manager instance, to avoid leak sanitizers
    // complaining about the entries in Manager::fmap being leaked at program
    // exit.
    static Manager* inst = new Manager();
    return inst;
  }
};

Registry& Registry::set_body(PackedFunc f) {  // NOLINT(*)
  func_ = f;
  return *this;
}

Registry& Registry::Register(const std::string& name, bool override) {  // NOLINT(*)
  Manager* m = Manager::Global();
  std::lock_guard<std::mutex> lock(m->mutex);
  auto it = m->fmap.find(name);
  if (it == m->fmap.end()) {
    Registry* r = new Registry();
    r->name_ = name;
    m->fmap[name] = r;
    return *r;
  } else {
    CHECK(override)
      << "Global PackedFunc " << name << " is already registered";
    return *it->second;
  }
}

bool Registry::Remove(const std::string& name) {
  Manager* m = Manager::Global();
  std::lock_guard<std::mutex> lock(m->mutex);
  auto it = m->fmap.find(name);
  if (it == m->fmap.end()) return false;
  m->fmap.erase(it);
  return true;
}

const PackedFunc* Registry::Get(const std::string& name) {
  Manager* m = Manager::Global();
  std::lock_guard<std::mutex> lock(m->mutex);
  auto it = m->fmap.find(name);
  if (it == m->fmap.end()) return nullptr;
  return &(it->second->func_);
}

std::vector<std::string> Registry::ListNames() {
  Manager* m = Manager::Global();
  std::lock_guard<std::mutex> lock(m->mutex);
  std::vector<std::string> keys;
  keys.reserve(m->fmap.size());
  for (const auto &kv : m->fmap) {
    keys.push_back(kv.first);
  }
  return keys;
}

ExtTypeVTable* ExtTypeVTable::Get(int type_code) {
  CHECK(type_code > kExtBegin && type_code < kExtEnd);
  Registry::Manager* m = Registry::Manager::Global();
  ExtTypeVTable* vt = &(m->ext_vtable[type_code]);
  CHECK(vt->destroy != nullptr)
      << "Extension type not registered";
  return vt;
}

ExtTypeVTable* ExtTypeVTable::RegisterInternal(
    int type_code, const ExtTypeVTable& vt) {
  CHECK(type_code > kExtBegin && type_code < kExtEnd);
  Registry::Manager* m = Registry::Manager::Global();
  std::lock_guard<std::mutex> lock(m->mutex);
  ExtTypeVTable* pvt = &(m->ext_vtable[type_code]);
  pvt[0] = vt;
  return pvt;
}
}  // namespace runtime
}  // namespace decord

/*! \brief entry to to easily hold returning information */
struct DECORDFuncThreadLocalEntry {
  /*! \brief result holder for returning strings */
  std::vector<std::string> ret_vec_str;
  /*! \brief result holder for returning string pointers */
  std::vector<const char *> ret_vec_charp;
};

/*! \brief Thread local store that can be used to hold return values. */
typedef dmlc::ThreadLocalStore<DECORDFuncThreadLocalEntry> DECORDFuncThreadLocalStore;

int DECORDExtTypeFree(void* handle, int type_code) {
  API_BEGIN();
  decord::runtime::ExtTypeVTable::Get(type_code)->destroy(handle);
  API_END();
}

int DECORDFuncRegisterGlobal(
    const char* name, DECORDFunctionHandle f, int override) {
  API_BEGIN();
  decord::runtime::Registry::Register(name, override != 0)
      .set_body(*static_cast<decord::runtime::PackedFunc*>(f));
  API_END();
}

int DECORDFuncGetGlobal(const char* name, DECORDFunctionHandle* out) {
  API_BEGIN();
  const decord::runtime::PackedFunc* fp =
      decord::runtime::Registry::Get(name);
  if (fp != nullptr) {
    *out = new decord::runtime::PackedFunc(*fp);  // NOLINT(*)
  } else {
    *out = nullptr;
  }
  API_END();
}

int DECORDFuncListGlobalNames(int *out_size,
                           const char*** out_array) {
  API_BEGIN();
  DECORDFuncThreadLocalEntry *ret = DECORDFuncThreadLocalStore::Get();
  ret->ret_vec_str = decord::runtime::Registry::ListNames();
  ret->ret_vec_charp.clear();
  for (size_t i = 0; i < ret->ret_vec_str.size(); ++i) {
    ret->ret_vec_charp.push_back(ret->ret_vec_str[i].c_str());
  }
  *out_array = dmlc::BeginPtr(ret->ret_vec_charp);
  *out_size = static_cast<int>(ret->ret_vec_str.size());
  API_END();
}
