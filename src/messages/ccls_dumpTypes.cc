// Copyright 2024 ccls-re Authors
// SPDX-License-Identifier: Apache-2.0

#include "message_handler.hh"
#include "pipeline.hh"
#include "query.hh"

#include <string>
#include <string_view>
#include <vector>

namespace ccls {
namespace {

// --- Request parameter ---

struct CcleDumpTypesParam {
  // Namespace prefixes to include, e.g. ["RE", "REX", "REL"]
  std::vector<std::string> namespaces;
  // Optional path prefix filter for source files (unused for now, reserved)
  std::string includePrefix;
};
REFLECT_STRUCT(CcleDumpTypesParam, namespaces, includePrefix);

// --- Response structures ---

struct CcleBaseInfo {
  std::string qualName;
  Usr usr = 0;
};
REFLECT_STRUCT(CcleBaseInfo, qualName, usr);

struct CcleFieldInfo {
  std::string name;
  int64_t offset = -1;  // bytes
  int32_t size = -1;    // bytes
  std::string qualType;
  Usr typeUsr = 0;
};
REFLECT_STRUCT(CcleFieldInfo, name, offset, size, qualType, typeUsr);

struct CcleMethodInfo {
  std::string qualName;
  std::string shortName;
  std::string signature;
  bool isVirtual = false;
  bool isPure = false;
  bool isStatic = false;
  int32_t vtableIndex = -1;
  Usr usr = 0;
};
REFLECT_STRUCT(CcleMethodInfo, qualName, shortName, signature, isVirtual, isPure, isStatic,
               vtableIndex, usr);

struct CcleRecordInfo {
  std::string qualName;
  std::string shortName;
  Usr usr = 0;
  int32_t size = -1;
  int32_t align = -1;
  bool hasVTable = false;
  std::vector<CcleBaseInfo> bases;
  std::vector<CcleFieldInfo> fields;
  std::vector<CcleMethodInfo> methods;
};
REFLECT_STRUCT(CcleRecordInfo, qualName, shortName, usr, size, align, hasVTable,
               bases, fields, methods);

struct CcleEnumValueInfo {
  std::string name;
  std::string value;  // string to handle both signed and unsigned
};
REFLECT_STRUCT(CcleEnumValueInfo, name, value);

struct CcleEnumInfo {
  std::string qualName;
  std::string shortName;
  Usr usr = 0;
  std::string underlyingType;
  int32_t size = -1;
  bool scoped = false;
  std::vector<CcleEnumValueInfo> values;
};
REFLECT_STRUCT(CcleEnumInfo, qualName, shortName, usr, underlyingType, size,
               scoped, values);

struct CcleTypedefInfo {
  std::string qualName;
  std::string shortName;
  Usr usr = 0;
  Usr targetUsr = 0;
  std::string targetQualName;
  std::string underlyingType;
};
REFLECT_STRUCT(CcleTypedefInfo, qualName, shortName, usr, targetUsr,
               targetQualName, underlyingType);

struct CcleDumpTypesResult {
  std::vector<CcleRecordInfo> records;
  std::vector<CcleEnumInfo> enums;
  std::vector<CcleTypedefInfo> typedefs;
};
REFLECT_STRUCT(CcleDumpTypesResult, records, enums, typedefs);

// Check if a qualified name matches any of the requested namespace prefixes
bool matchesNamespace(std::string_view qname,
                      const std::vector<std::string> &namespaces) {
  if (namespaces.empty())
    return true;
  for (const auto &ns : namespaces) {
    if (qname.size() > ns.size() + 2 &&
        qname.substr(0, ns.size()) == ns &&
        qname[ns.size()] == ':' && qname[ns.size() + 1] == ':')
      return true;
  }
  return false;
}

} // namespace

void MessageHandler::ccls_dumpTypes(JsonReader &reader, ReplyOnce &reply) {
  CcleDumpTypesParam param;
  reflect(reader, param);

  CcleDumpTypesResult result;

  for (auto &type : db->types) {
    const auto *def = type.anyDef();
    if (!def)
      continue;

    std::string qname(def->name(true));

    if (!matchesNamespace(qname, param.namespaces))
      continue;

    std::string sname(def->name(false));

    // Records (Class / Struct)
    if ((def->kind == SymbolKind::Struct || def->kind == SymbolKind::Class) &&
        !def->alias_of) {
      CcleRecordInfo rec;
      rec.qualName = qname;
      rec.shortName = sname;
      rec.usr = type.usr;
      rec.size = def->record_size;
      rec.align = def->record_align;
      rec.hasVTable = def->has_vtable;

      // Bases
      for (Usr base_usr : def->bases) {
        if (!db->hasType(base_usr))
          continue;
        const auto *base_def = db->getType(base_usr).anyDef();
        if (!base_def)
          continue;
        CcleBaseInfo bi;
        bi.qualName = std::string(base_def->name(true));
        bi.usr = base_usr;
        rec.bases.push_back(std::move(bi));
      }

      // Fields
      for (auto [var_usr, offset] : def->vars) {
        if (!db->hasVar(var_usr))
          continue;
        const auto *var_def = db->getVar(var_usr).anyDef();
        if (!var_def)
          continue;
        CcleFieldInfo fi;
        fi.name = std::string(var_def->name(false));
        fi.offset = offset >= 0 ? offset / 8 : -1; // bits to bytes
        fi.size = var_def->type_size;
        fi.qualType = var_def->type_str;
        fi.typeUsr = var_def->type;
        rec.fields.push_back(std::move(fi));
      }

      // Methods
      for (Usr func_usr : def->funcs) {
        if (!db->hasFunc(func_usr))
          continue;
        const auto *func_def = db->getFunc(func_usr).anyDef();
        if (!func_def)
          continue;
        CcleMethodInfo mi;
        mi.qualName = std::string(func_def->name(true));
        mi.shortName = std::string(func_def->name(false));
        mi.signature = std::string(func_def->detailed_name);
        mi.isVirtual = func_def->is_virtual;
        mi.isPure = func_def->is_pure;
        mi.isStatic = func_def->storage == clang::SC_Static;
        mi.vtableIndex = func_def->vtable_index;
        mi.usr = func_usr;
        rec.methods.push_back(std::move(mi));
      }

      result.records.push_back(std::move(rec));
      continue;
    }

    // Enums
    if (def->kind == SymbolKind::Enum) {
      CcleEnumInfo ei;
      ei.qualName = qname;
      ei.shortName = sname;
      ei.usr = type.usr;
      ei.underlyingType = def->enum_underlying_type;
      ei.size = def->enum_size;
      ei.scoped = def->enum_scoped;

      for (const auto &ev : def->enum_values) {
        CcleEnumValueInfo evi;
        evi.name = ev.name;
        if (ev.is_unsigned)
          evi.value = std::to_string(static_cast<uint64_t>(ev.value));
        else
          evi.value = std::to_string(ev.value);
        ei.values.push_back(std::move(evi));
      }

      result.enums.push_back(std::move(ei));
      continue;
    }

    // Typedefs
    if (def->alias_of || (def->typedef_underlying && def->typedef_underlying[0])) {
      CcleTypedefInfo ti;
      ti.qualName = qname;
      ti.shortName = sname;
      ti.usr = type.usr;
      ti.targetUsr = def->alias_of;
      ti.underlyingType = def->typedef_underlying ? def->typedef_underlying : "";
      if (def->alias_of && db->hasType(def->alias_of)) {
        const auto *target_def = db->getType(def->alias_of).anyDef();
        if (target_def)
          ti.targetQualName = std::string(target_def->name(true));
      }
      result.typedefs.push_back(std::move(ti));
    }
  }

  reply(result);
}

} // namespace ccls
