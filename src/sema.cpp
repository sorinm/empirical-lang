/*
 * Semantic Analysis -- type checking and identifier resolution
 *
 * Copyright (C) 2019 Empirical Software Solutions, LLC
 *
 * This program is distributed under the terms of the GNU Affero General
 * Public License with the Commons Clause.
 *
 */

#include <vector>
#include <stack>
#include <sstream>
#include <iostream>
#include <unordered_map>

#include <empirical.hpp>

#include <VVM/types.h>
#include <VVM/opcodes.h>
#include <VVM/utils/csv_infer.hpp>

// build high-level IR (HIR) from abstract syntax tree (AST)
class SemaVisitor : public AST::BaseVisitor {
  // store all prior IR
  std::vector<HIR::stmt_t> history_;

  /* get info from a node */

  // return resolved item's type, or nullptr if not available
  HIR::datatype_t get_type(HIR::resolved_t node) {
    if (node == nullptr) {
      return nullptr;
    }
    switch (node->resolved_kind) {
      case HIR::resolved_::ResolvedKind::kDeclRef: {
        HIR::DeclRef_t ptr = dynamic_cast<HIR::DeclRef_t>(node);
        return ptr->ref->type;
      }
      case HIR::resolved_::ResolvedKind::kFuncRef: {
        HIR::FuncRef_t ptr = dynamic_cast<HIR::FuncRef_t>(node);
        HIR::FunctionDef_t def = dynamic_cast<HIR::FunctionDef_t>(ptr->ref);
        return get_type(def);
      }
      case HIR::resolved_::ResolvedKind::kGenericFuncRef: {
        HIR::GenericFuncRef_t ptr = dynamic_cast<HIR::GenericFuncRef_t>(node);
        HIR::GenericFunctionDef_t def =
          dynamic_cast<HIR::GenericFunctionDef_t>(ptr->ref);
        HIR::FunctionDef_t original =
          dynamic_cast<HIR::FunctionDef_t>(def->original_func);
        return get_type(original);
      }
      case HIR::resolved_::ResolvedKind::kDataRef: {
        HIR::DataRef_t dr = dynamic_cast<HIR::DataRef_t>(node);
        HIR::DataDef_t dd = dynamic_cast<HIR::DataDef_t>(dr->ref);
        return HIR::Kind(HIR::UDT(dd->name, node));
      }
      case HIR::resolved_::ResolvedKind::kModRef: {
        return nullptr;
      }
      case HIR::resolved_::ResolvedKind::kVVMOpRef: {
        HIR::VVMOpRef_t ptr = dynamic_cast<HIR::VVMOpRef_t>(node);
        return ptr->type;
      }
      case HIR::resolved_::ResolvedKind::kVVMTypeRef: {
        HIR::VVMTypeRef_t ptr = dynamic_cast<HIR::VVMTypeRef_t>(node);
        return HIR::Kind(HIR::VVMType(ptr->t));
      }
      case HIR::resolved_::ResolvedKind::kCompilerRef: {
        HIR::CompilerRef_t ptr = dynamic_cast<HIR::CompilerRef_t>(node);
        return ptr->type;
      }
    }
  }

  // return type from a function definition
  HIR::datatype_t get_type(HIR::FunctionDef_t node) {
    std::vector<HIR::datatype_t> argtypes;
    for (HIR::declaration_t arg: node->args) {
      argtypes.push_back(arg->type);
    }
    HIR::datatype_t rettype = node->rettype;
    return FuncType(argtypes, rettype);
  }

  // return resolved item's scope, or zero if not available
  size_t get_scope(HIR::resolved_t node) {
    if (node == nullptr) {
      return 0;
    }
    size_t scope = 0;
    switch (node->resolved_kind) {
      case HIR::resolved_::ResolvedKind::kDataRef: {
        HIR::DataRef_t dr = dynamic_cast<HIR::DataRef_t>(node);
        HIR::DataDef_t dd = dynamic_cast<HIR::DataDef_t>(dr->ref);
        scope = dd->scope;
        break;
      }
      default:
        break;
    }
    return scope;
  }

  // get type's scope, or zero if not available
  size_t get_scope(HIR::datatype_t node) {
    if (node == nullptr) {
      return 0;
    }
    size_t scope = 0;
    switch (node->datatype_kind) {
      case HIR::datatype_::DatatypeKind::kUDT: {
        HIR::UDT_t udt = dynamic_cast<HIR::UDT_t>(node);
        scope = get_scope(udt->ref);
        break;
      }
      default:
        break;
    }
    return scope;
  }

  // get underlying data definition from a user-defined type
  HIR::DataDef_t get_data_def(HIR::datatype_t node) {
    switch (node->datatype_kind) {
      case HIR::datatype_::DatatypeKind::kUDT: {
        HIR::UDT_t udt = dynamic_cast<HIR::UDT_t>(node);
        HIR::DataRef_t dr = dynamic_cast<HIR::DataRef_t>(udt->ref);
        HIR::DataDef_t dd = dynamic_cast<HIR::DataDef_t>(dr->ref);
        return dd;
      }
      default:
        return nullptr;
    }
  }

  /* symbol resolution */

  // symbol map for a single scope
  struct Scope {
    typedef std::vector<HIR::resolved_t> Resolveds;
    typedef std::unordered_map<std::string, Resolveds> Map;
    Map map;
    const size_t previous_scope;
    Scope(size_t prev): previous_scope(prev) {}
  };

  // symbol resolution table
  std::vector<Scope> symbol_table_;
  size_t current_scope_;
  HIR::expr_t preferred_scope_;

  // return array of pointers to HIR nodes where symbol was declared
  Scope::Resolveds find_symbol(const std::string& symbol,
                               bool* in_preferred = nullptr) {
    // check the preferred scope first
    if (preferred_scope_ != nullptr) {
      size_t idx = get_scope(preferred_scope_->type);
      Scope::Resolveds initial = find_symbol_in_scope(symbol, idx);
      if (!initial.empty()) {
        if (in_preferred != nullptr) {
          *in_preferred = true;
        }
        return initial;
      }
    }
    if (in_preferred != nullptr) {
      *in_preferred = false;
    }

    // iteratively check current and prior scopes
    size_t i = current_scope_;
    bool done = false;
    while (!done) {
      Scope& scope = symbol_table_[i];
      auto symbol_iter = scope.map.find(symbol);
      if (symbol_iter != scope.map.end()) {
        return symbol_iter->second;
      }
      if (i != 0) {
        i = scope.previous_scope;
      }
      else {
        done = true;
      }
    }
    return Scope::Resolveds();
  }

  // search only the given scope
  Scope::Resolveds find_symbol_in_scope(const std::string& symbol,
                                        size_t idx) {
    Scope& scope = symbol_table_[idx];
    auto symbol_iter = scope.map.find(symbol);
    if (symbol_iter != scope.map.end()) {
      return symbol_iter->second;
    }
    return Scope::Resolveds();
  }

  // save pointer to HIR node for symbol; return false if already there
  bool store_symbol(const std::string& symbol, HIR::resolved_t ptr) {
    Scope& scope = symbol_table_[current_scope_];
    auto iter = scope.map.find(symbol);
    if (iter != scope.map.end()) {
      // check that we can overload if the symbol already exists
      auto& resolveds = iter->second;
      for (size_t i = 0; i < resolveds.size(); i++) {
        if (!is_overloadable(resolveds[i], ptr)) {
          if (interactive_ && is_overridable(resolveds[i])) {
            resolveds[i] = ptr;
            return true;
          }
          else {
            return false;
          }
        }
      }
      resolveds.push_back(ptr);
    }
    else {
      scope.map.emplace(symbol, std::initializer_list<HIR::resolved_t>{ptr});
    }
    return true;
  }

  // remove symbol from current scope; return false if not found
  bool remove_symbol(const std::string& symbol) {
    Scope& scope = symbol_table_[current_scope_];
    return scope.map.erase(symbol) > 0;
  }

  // remove symbol reference---needed to unwind scope during errors
  void remove_symbol_ref(const std::string& symbol, HIR::resolved_t ptr) {
    Scope& scope = symbol_table_[current_scope_];
    auto iter = scope.map.find(symbol);
    if (iter != scope.map.end()) {
      auto& resolveds = iter->second;
      for (size_t i = 0; i < resolveds.size(); i++) {
        if (resolveds[i] == ptr) {
          resolveds.erase(resolveds.begin() + i);
          break;
        }
      }
    }
  }

  // activate a new scope
  void push_scope() {
    symbol_table_.push_back(Scope(current_scope_));
    current_scope_ = symbol_table_.size() - 1;
  }

  // deactivate current scope
  void pop_scope() {
    current_scope_ = symbol_table_[current_scope_].previous_scope;
  }

  /* type check */

  // list of return types for each function definition in stack
  std::stack<std::vector<HIR::datatype_t>> rettype_stack_;

  // string-ify a datatype
  std::string to_string(HIR::datatype_t node) {
    if (node == nullptr) {
      return "_";
    }
    switch (node->datatype_kind) {
      case HIR::datatype_::DatatypeKind::kVVMType: {
        HIR::VVMType_t b = dynamic_cast<HIR::VVMType_t>(node);
        return VVM::empirical_type_strings[b->t];
      }
      case HIR::datatype_::DatatypeKind::kUDT: {
        HIR::UDT_t udt = dynamic_cast<HIR::UDT_t>(node);
        return udt->s;
      }
      case HIR::datatype_::DatatypeKind::kArray: {
        HIR::Array_t df = dynamic_cast<HIR::Array_t>(node);
        return "[" + to_string(df->type) + "]";
      }
      case HIR::datatype_::DatatypeKind::kFuncType: {
        HIR::FuncType_t ft = dynamic_cast<HIR::FuncType_t>(node);
        std::string result = "(";
        if (ft->argtypes.size() >= 1) {
          result += to_string(ft->argtypes[0]);
          for (size_t i = 1; i < ft->argtypes.size(); i++) {
            result += ", " + to_string(ft->argtypes[i]);
          }
        }
        result += ") -> " + to_string(ft->rettype);
        return result;
      }
      case HIR::datatype_::DatatypeKind::kKind: {
        HIR::Kind_t k = dynamic_cast<HIR::Kind_t>(node);
        return "Kind(" + to_string(k->type) + ")";
      }
      case HIR::datatype_::DatatypeKind::kVoid: {
        return "()";
      }
    }
  }

  // string-ify the underlying values of a UDT
  std::string to_string_udt(HIR::datatype_t node) {
    std::string result;
    if (node != nullptr &&
        node->datatype_kind == HIR::datatype_::DatatypeKind::kUDT) {
      HIR::DataDef_t dd = get_data_def(node);
      result = "(";
      for (size_t i = 0; i < dd->body.size(); i++) {
        if (i > 0) {
          result += ", ";
        }
        result += to_string(dd->body[i]->type);
      }
      result += ")";
    }
    return result;
  }

  // validate that two types have the same underlying structure
  bool is_same_type(HIR::datatype_t left, HIR::datatype_t right) {
    if (left == nullptr || right == nullptr) {
      return true;
    }
    if (left->datatype_kind != right->datatype_kind) {
      return false;
    }
    switch (left->datatype_kind) {
      case HIR::datatype_::DatatypeKind::kVVMType: {
        HIR::VVMType_t left_b = dynamic_cast<HIR::VVMType_t>(left);
        HIR::VVMType_t right_b = dynamic_cast<HIR::VVMType_t>(right);
        return left_b->t == right_b->t;
      }
      case HIR::datatype_::DatatypeKind::kUDT: {
        HIR::DataDef_t left_dd = get_data_def(left);
        HIR::DataDef_t right_dd = get_data_def(right);
        if (left_dd->body.size() != right_dd->body.size()) {
          return false;
        }
        for (size_t i = 0; i < left_dd->body.size(); i++) {
          if (!is_same_type(left_dd->body[i]->type,
                            right_dd->body[i]->type) ||
              (left_dd->body[i]->name != right_dd->body[i]->name)) {
            return false;
          }
        }
        break;
      }
      case HIR::datatype_::DatatypeKind::kArray: {
        HIR::Array_t left_arr = dynamic_cast<HIR::Array_t>(left);
        HIR::Array_t right_arr = dynamic_cast<HIR::Array_t>(right);
        if (!is_same_type(left_arr->type, right_arr->type)) {
          return false;
        }
        break;
      }
      case HIR::datatype_::DatatypeKind::kFuncType: {
        HIR::FuncType_t left_ft = dynamic_cast<HIR::FuncType_t>(left);
        HIR::FuncType_t right_ft = dynamic_cast<HIR::FuncType_t>(right);
        if (left_ft->argtypes.size() != right_ft->argtypes.size()) {
          return false;
        }
        for (size_t i = 0; i < left_ft->argtypes.size(); i++) {
          if (!is_same_type(left_ft->argtypes[i], right_ft->argtypes[i])) {
            return false;
          }
        }
        if (!is_same_type(left_ft->rettype, right_ft->rettype)) {
          return false;
        }
        break;
      }
      case HIR::datatype_::DatatypeKind::kKind: {
        HIR::Kind_t leftk = dynamic_cast<HIR::Kind_t>(left);
        HIR::Kind_t rightk = dynamic_cast<HIR::Kind_t>(right);
        return is_same_type(leftk->type, rightk->type);
      }
      case HIR::datatype_::DatatypeKind::kVoid: {
        return true;
      }
    }
    return true;
  }

  // ensure instantiated structure reflects array-ized underlying structure
  bool is_dataframe_type_valid(HIR::DataDef_t left, HIR::resolved_t ref) {
    HIR::DataRef_t dr = dynamic_cast<HIR::DataRef_t>(ref);
    HIR::DataDef_t right = dynamic_cast<HIR::DataDef_t>(dr->ref);

    if (left->body.size() != right->body.size()) {
      return false;
    }
    for (size_t i = 0; i < left->body.size(); i++) {
      if (!is_same_type(HIR::Array(left->body[i]->type),
                        right->body[i]->type) ||
          (left->body[i]->name != right->body[i]->name)) {
        return false;
      }
    }
    return true;
  }

  // find scalar UDT for a Dataframe name (assumes leading '!')
  HIR::DataDef_t get_underlying_udt(std::string name) {
    std::string underlying_name = name.c_str() + 1;
    Scope::Resolveds underlying_resolveds = find_symbol(underlying_name);
    if (underlying_resolveds.empty()) {
      return nullptr;
    }
    HIR::resolved_t underlying_ref = underlying_resolveds[0];
    if (underlying_ref->resolved_kind !=
        HIR::resolved_::ResolvedKind::kDataRef) {
      return nullptr;
    }
    HIR::DataRef_t dr = dynamic_cast<HIR::DataRef_t>(underlying_ref);
    HIR::DataDef_t dd = dynamic_cast<HIR::DataDef_t>(dr->ref);
    return dd;
  }

  // attempt to make Dataframe with the given type name
  HIR::datatype_t make_dataframe(std::string name) {
    // find underlying data definition first
    HIR::DataDef_t node = get_underlying_udt(name);
    if (node == nullptr) {
      return nullptr;
    }

    // see if the Dataframe already exists
    HIR::resolved_t ref = nullptr;
    Scope::Resolveds resolveds = find_symbol(name);
    if (resolveds.size() != 0) {
      // ensure underlying hasn't changed
      ref = resolveds[0];
      if (!is_dataframe_type_valid(node, ref)) {
        ref = nullptr;
      }
    }
    if (ref == nullptr) {
      // make Dataframe definition
      std::vector<HIR::declaration_t> body;
      push_scope();
      size_t scope = current_scope_;
      for (HIR::declaration_t b: node->body) {
        auto d = HIR::declaration(b->name, nullptr, b->value,
                                  HIR::Array(b->type), b->offset);
        store_symbol(b->name, HIR::DeclRef(d));
        body.push_back(d);
      }
      pop_scope();
      HIR::stmt_t new_node = HIR::DataDef(name, body, scope);
      ref = HIR::DataRef(new_node);
      store_symbol(name, ref);
    }
    return HIR::UDT(name, ref);
  }

  bool is_string_type(HIR::datatype_t node) {
    if (node != nullptr &&
        node->datatype_kind == HIR::datatype_::DatatypeKind::kVVMType) {
      HIR::VVMType_t b = dynamic_cast<HIR::VVMType_t>(node);
      return b->t == size_t(VVM::vvm_types::Ss);
    }
    return false;
  }

  bool is_indexable_type(HIR::datatype_t node) {
    if (node != nullptr &&
        node->datatype_kind == HIR::datatype_::DatatypeKind::kVVMType) {
      HIR::VVMType_t b = dynamic_cast<HIR::VVMType_t>(node);
      return b->t == size_t(VVM::vvm_types::i64s);
    }
    return false;
  }

  bool is_boolean_type(HIR::datatype_t node) {
    if (node != nullptr &&
        node->datatype_kind == HIR::datatype_::DatatypeKind::kVVMType) {
      HIR::VVMType_t b = dynamic_cast<HIR::VVMType_t>(node);
      return b->t == size_t(VVM::vvm_types::b8s);
    }
    return false;
  }

  bool is_dataframe_type(HIR::datatype_t node) {
    if (node != nullptr &&
        node->datatype_kind == HIR::datatype_::DatatypeKind::kUDT) {
      HIR::UDT_t udt = dynamic_cast<HIR::UDT_t>(node);
      return udt->s[0] == '!';
    }
    return false;
  }

  bool is_array_type(HIR::datatype_t node) {
    return (node != nullptr &&
            node->datatype_kind == HIR::datatype_::DatatypeKind::kArray);
  }

  // can overload types and functions with new functions
  bool is_overloadable(HIR::resolved_t first, HIR::resolved_t second) {
    switch (first->resolved_kind) {
      // overlaod types with functions
      case HIR::resolved_::ResolvedKind::kVVMTypeRef:
      case HIR::resolved_::ResolvedKind::kDataRef: {
        switch (second->resolved_kind) {
          case HIR::resolved_::ResolvedKind::kVVMOpRef:
          case HIR::resolved_::ResolvedKind::kFuncRef:
            return true;
          default:
            return false;
        }
      }
      // overload functions with unique signatures
      case HIR::resolved_::ResolvedKind::kVVMOpRef:
      case HIR::resolved_::ResolvedKind::kFuncRef: {
        switch (second->resolved_kind) {
          case HIR::resolved_::ResolvedKind::kVVMOpRef:
          case HIR::resolved_::ResolvedKind::kFuncRef:
            return !is_same_type(get_type(first), get_type(second));
          default:
            return false;
        }
      }
      default:
        return false;
    }
  }

  // can override anything that isn't builtin
  bool is_overridable(HIR::resolved_t ref) {
    if (ref == nullptr) {
      return true;
    }
    switch (ref->resolved_kind) {
      case HIR::resolved_::ResolvedKind::kVVMTypeRef:
      case HIR::resolved_::ResolvedKind::kVVMOpRef:
        return false;
      default:
        return true;
    }
  }

  // can call functions and types (casts)
  bool is_callable(HIR::datatype_t node) {
    if (node == nullptr) {
      return true;
    }
    switch (node->datatype_kind) {
      case HIR::datatype_::DatatypeKind::kFuncType:
      case HIR::datatype_::DatatypeKind::kKind:
        return true;
      default:
        return false;
    }
  }

  bool is_overloaded(HIR::expr_t node) {
    return (node != nullptr &&
            node->expr_kind == HIR::expr_::ExprKind::kOverloadedId);
  }

  bool is_expr(HIR::stmt_t node) {
    return (node != nullptr &&
            node->stmt_kind == HIR::stmt_::StmtKind::kExpr);
  }

  bool is_slice(HIR::slice_t node) {
    return (node != nullptr &&
            node->slice_kind == HIR::slice_::SliceKind::kSlice);
  }

  bool is_kind_type(HIR::datatype_t node) {
    return (node != nullptr &&
            node->datatype_kind == HIR::datatype_::DatatypeKind::kKind);
  }

  bool is_void_type(HIR::datatype_t node) {
    return (node != nullptr &&
            node->datatype_kind == HIR::datatype_::DatatypeKind::kVoid);
  }

  // expressions are temporary if they do not outlive their immediate use
  bool is_temporary(HIR::expr_t node) {
    if (node != nullptr) {
      switch(node->expr_kind) {
        case HIR::expr_::ExprKind::kMember:
        case HIR::expr_::ExprKind::kSubscript:
        case HIR::expr_::ExprKind::kId:
        case HIR::expr_::ExprKind::kImpliedMember:
        case HIR::expr_::ExprKind::kOverloadedId:
          return false;
        default:
          break;
      }
    }
    return true;
  }

  // return underlying type from higher kinds
  HIR::datatype_t get_underlying_type(HIR::datatype_t node) {
    if (node == nullptr) {
      return nullptr;
    }
    switch (node->datatype_kind) {
      case HIR::datatype_::DatatypeKind::kArray: {
        HIR::Array_t arr = dynamic_cast<HIR::Array_t>(node);
        return arr->type;
      }
      case HIR::datatype_::DatatypeKind::kKind: {
        HIR::Kind_t k = dynamic_cast<HIR::Kind_t>(node);
        return k->type;
      }
      default:
        return nullptr;
    }
  }

  // return generic function from reference
  HIR::GenericFunctionDef_t get_generic(HIR::expr_t node) {
    if (node == nullptr) {
      return nullptr;
    }
    switch (node->expr_kind) {
      case HIR::expr_::ExprKind::kId: {
        HIR::Id_t id = dynamic_cast<HIR::Id_t>(node);
        HIR::resolved_t ref = id->ref;
        if (ref == nullptr) {
          return nullptr;
        }
        switch (ref->resolved_kind) {
          case HIR::resolved_::ResolvedKind::kGenericFuncRef: {
            HIR::GenericFuncRef_t func =
              dynamic_cast<HIR::GenericFuncRef_t>(ref);
            HIR::GenericFunctionDef_t def =
              dynamic_cast<HIR::GenericFunctionDef_t>(func->ref);
            return def;
          }
          default:
            return nullptr;
        }
      }
      default:
         return nullptr;
    }
  }

  // return function's argument types
  std::vector<HIR::datatype_t> get_argtypes(HIR::datatype_t node) {
    if (node == nullptr) {
      return std::vector<HIR::datatype_t>();
    }
    switch (node->datatype_kind) {
      case HIR::datatype_::DatatypeKind::kFuncType: {
        HIR::FuncType_t ft = dynamic_cast<HIR::FuncType_t>(node);
        return ft->argtypes;
      }
      case HIR::datatype_::DatatypeKind::kKind: {
        HIR::Kind_t k = dynamic_cast<HIR::Kind_t>(node);
        std::vector<HIR::datatype_t> argtypes;
        HIR::DataDef_t dd = get_data_def(k->type);
        if (dd != nullptr) {
          for (HIR::declaration_t d: dd->body) {
            argtypes.push_back(d->type);
          }
        }
        return argtypes;
      }
      default:
        return std::vector<HIR::datatype_t>();
    }
  }

  // return function's return type
  HIR::datatype_t get_rettype(HIR::datatype_t node) {
    if (node == nullptr) {
      return nullptr;
    }
    switch (node->datatype_kind) {
      case HIR::datatype_::DatatypeKind::kFuncType: {
        HIR::FuncType_t ft = dynamic_cast<HIR::FuncType_t>(node);
        return ft->rettype;
      }
      case HIR::datatype_::DatatypeKind::kKind: {
        HIR::Kind_t k = dynamic_cast<HIR::Kind_t>(node);
        return k->type;
      }
      default:
         return nullptr;
    }
  }

  // return explanation of why function arguments didn't match
  std::string match_args(const std::vector<HIR::expr_t>& args,
                         HIR::datatype_t func_type) {
    if (func_type == nullptr) {
      return std::string();
    }
    std::ostringstream oss;
    std::vector<HIR::datatype_t> argtypes = get_argtypes(func_type);
    if (args.size() != argtypes.size()) {
      oss << "wrong number of arguments; expected " << argtypes.size()
          << " but got " << args.size();
    }
    else {
      for (size_t i = 0; i < args.size(); i++) {
        if (!is_same_type(args[i]->type, argtypes[i])) {
          oss << "argument type at position " << i << " does not match: "
              << to_string(args[i]->type) << " vs " << to_string(argtypes[i]);
          break;
        }
      }
    }
    return oss.str();
  }

  // create an anonymous function name
  std::string anon_func_name() {
    static size_t counter = 0;
    std::ostringstream oss;
    oss << "Anon__" << counter++;
    return oss.str();
  }

  // return HIR node for a type definition string
  HIR::stmt_t create_datatype(const std::string& type_name,
                              const std::string& type_def) {
    std::string data_str = "data Anon: " + type_def + " end";
    AST::mod_t ast = parse(data_str, false, false);
    AST::Module_t mod = dynamic_cast<AST::Module_t>(ast);
    AST::stmt_t parsed = mod->body[0];
    AST::DataDef_t dd = dynamic_cast<AST::DataDef_t>(parsed);
    dd->name = type_name;
    return visit(parsed);
  }

  // return a type definition string from aliases
  std::string get_type_string(std::vector<HIR::alias_t> aliases) {
    std::string result;
    for (HIR::alias_t a: aliases) {
      std::string name = a->name.empty() ? a->value->name : a->name;
      HIR::datatype_t dt = is_array_type(a->value->type) ?
                             get_underlying_type(a->value->type) :
                             a->value->type;
      std::string new_item = name + ": " + to_string(dt);
      if (result.empty()) {
        result = new_item;
      }
      else {
        result += ", " + new_item;
      }
    }
    return result;
  }

  // return a type definition string from datatype
  std::string get_type_string(HIR::datatype_t node) {
    std::string result;
    HIR::DataDef_t dd = get_data_def(node);
    for (HIR::declaration_t d: dd->body) {
      HIR::datatype_t dt = is_array_type(d->type) ?
                             get_underlying_type(d->type) :
                             d->type;
      std::string new_item = d->name + ": " + to_string(dt);
      if (result.empty()) {
        result = new_item;
      }
      else {
        result += ", " + new_item;
      }
    }
    return result;
  }

  // drop a set of columns from a Dataframe; return string for further work
  std::string drop_columns(HIR::datatype_t orig_type,
                           HIR::datatype_t drop_type,
                           const std::string& extra) {
    HIR::DataDef_t orig_dd = get_data_def(orig_type);

    // store dropped names for easy look-up
    std::unordered_set<std::string> dropped_names;
    if (drop_type != nullptr) {
      HIR::DataDef_t drop_dd = get_data_def(drop_type);
      for (HIR::declaration_t d: drop_dd->body) {
        dropped_names.insert(d->name);
      }
    }
    if (!extra.empty()) {
      dropped_names.insert(extra);
    }

    // build string from entries that aren't among the dropped names
    std::string result;
    for (HIR::declaration_t d: orig_dd->body) {
      if (dropped_names.find(d->name) == dropped_names.end()) {
        HIR::datatype_t dt = is_array_type(d->type) ?
                               get_underlying_type(d->type) :
                               d->type;
        std::string new_item = d->name + ": " + to_string(dt);
        if (result.empty()) {
          result = new_item;
        }
        else {
          result += ", " + new_item;
        }
      }
    }
    return result;
  }

  /* builtin items */

  // save all builtin items so that id resolution will find them
  void save_builtins() {
    store_symbol("store", HIR::CompilerRef(size_t(CompilerCodes::kStore),
      HIR::FuncType({nullptr, HIR::VVMType(size_t(VVM::vvm_types::Ss))},
                    HIR::Void())));

#include <VVM/builtins.h>
  }

  /* miscellaneous */

  std::ostringstream sema_err_;

  bool interactive_;

  void nyi(const std::string& rule) const {
    std::string msg = "Not yet implemented: " + rule + '\n';
    throw std::logic_error(msg);
  }

 protected:

  antlrcpp::Any visitModule(AST::Module_t node) override {
    sema_err_.str("");
    sema_err_.clear();
    std::vector<HIR::stmt_t> results;
    for (AST::stmt_t s: node->body) {
      results.push_back(visit(s));
    }
    history_.insert(history_.end(), results.begin(), results.end());
    return HIR::Module(results, node->docstring);
  }

  antlrcpp::Any visitFunctionDef(AST::FunctionDef_t node) override {
    size_t starting_err_length = sema_err_.str().size();
    // get explicit return type
    HIR::expr_t explicit_rettype = nullptr;
    if (node->explicit_rettype) {
      explicit_rettype = visit(node->explicit_rettype);
    }
    HIR::datatype_t rettype = nullptr;
    if (explicit_rettype != nullptr) {
      if (is_kind_type(explicit_rettype->type)) {
        rettype = get_underlying_type(explicit_rettype->type);
      }
      else {
        sema_err_ << "Error: return type for " << node->name
                  << " has invalid type" << std::endl;
      }
    }
    // evaluate arguments in a new scope
    size_t outer_scope = current_scope_;
    push_scope();
    size_t inner_scope = current_scope_;
    std::vector<HIR::declaration_t> args;
    for (AST::declaration_t a: node->args) {
      args.push_back(visit(a));
    }
    // create shell now so body can have recursion
    std::vector<HIR::stmt_t> body;
    HIR::stmt_t new_node = HIR::FunctionDef(node->name, args, body,
                                            explicit_rettype, node->docstring,
                                            rettype);
    HIR::FunctionDef_t fd = dynamic_cast<HIR::FunctionDef_t>(new_node);
    // missing argument types implies generic function
    HIR::stmt_t generic = nullptr;
    for (AST::declaration_t a: node->args) {
      if (a->explicit_type == nullptr) {
        std::vector<HIR::stmt_t> instantiated;
        generic = HIR::GenericFunctionDef(new_node, instantiated);
        break;
      }
    }
    HIR::resolved_t ref = generic ? HIR::GenericFuncRef(generic)
                                  : HIR::FuncRef(new_node);
    // store name in outer scope
    current_scope_ = outer_scope;
    if (!store_symbol(node->name, ref)) {
      sema_err_ << "Error: symbol " << node->name
                << " was already defined" << std::endl;
    }
    // evaluate body in the inner scope
    current_scope_ = inner_scope;
    rettype_stack_.push(std::vector<HIR::datatype_t>());
    for (AST::stmt_t b: node->body) {
      body.push_back(visit(b));
    }
    fd->body = body;
    pop_scope();
    // get body's return type
    HIR::datatype_t body_rettype = nullptr;
    auto& rettypes = rettype_stack_.top();
    if (rettypes.empty()) {
      sema_err_ << "Error: function " << node->name
                << " has no return statements" << std::endl;
    }
    else {
      body_rettype = rettypes[0];
      for (size_t i = 1; i < rettypes.size(); i++) {
        if (!is_same_type(body_rettype, rettypes[i])) {
          sema_err_ << "Error: mismatched return types in function "
                    << node->name << ": " << to_string(body_rettype) << " vs "
                    << to_string(rettypes[i]) << std::endl;
        }
      }
    }
    rettype_stack_.pop();
    // infer return type if needed
    if (rettype == nullptr) {
      rettype = body_rettype;
    }
    if (rettype == nullptr) {
      sema_err_ << "Error: unable to determine return type for function "
                << node->name << std::endl;
    }
    if (!is_same_type(rettype, body_rettype)) {
      sema_err_ << "Error: mismatched return types: " << to_string(rettype)
                << " vs " << to_string(body_rettype) << std::endl;
    }
    // check if this had been a cast definition
    if (isupper(node->name[0])) {
      Scope::Resolveds resolveds = find_symbol(node->name);
      HIR::datatype_t cast_type = get_type(resolveds[0]);
      if (is_kind_type(cast_type)) {
        HIR::datatype_t expected = get_underlying_type(cast_type);
        if (!is_same_type(rettype, expected) &&
            !is_same_type(rettype, HIR::Array(expected))) {
          sema_err_ << "Error: cast definition for " << node->name
                    << " must return its own type" << std::endl;
        }
      } else {
        sema_err_ << "Error: cast definition must be for a type, not "
                  << node->name << std::endl;
      }
    }
    // remove from scope if an error had occured
    if (sema_err_.str().size() > starting_err_length) {
      remove_symbol_ref(node->name, ref);
    }
    // put everything together
    fd->rettype = rettype;
    return generic ? generic : new_node;
  }

  antlrcpp::Any visitDataDef(AST::DataDef_t node) override {
    size_t starting_err_length = sema_err_.str().size();
    if (islower(node->name[0])) {
      sema_err_ << "Error: type name " << node->name
                << " must begin with upper-case letter" << std::endl;
    }
    std::vector<HIR::declaration_t> body;
    HIR::stmt_t new_node = HIR::DataDef(node->name, body, 0);
    HIR::resolved_t ref = HIR::DataRef(new_node);
    if (!store_symbol(node->name, ref)) {
      sema_err_ << "Error: symbol " << node->name
                << " was already defined" << std::endl;
    }
    // evaluate body in new scope
    push_scope();
    size_t scope = current_scope_;
    size_t offset = 0;
    for (AST::declaration_t b: node->body) {
      HIR::declaration_t d = visit(b);
      d->offset = offset++;
      body.push_back(d);
    }
    pop_scope();
    // remove from scope if an error had occured
    if (sema_err_.str().size() > starting_err_length) {
      remove_symbol_ref(node->name, ref);
    }
    // put everything together
    HIR::DataDef_t dd = dynamic_cast<HIR::DataDef_t>(new_node);
    dd->body = body;
    dd->scope = scope;
    return new_node;
  }

  antlrcpp::Any visitReturn(AST::Return_t node) override {
    HIR::expr_t e = nullptr;
    if (node->value) {
      e = visit(node->value);
    }
    if (rettype_stack_.empty()) {
      sema_err_ << "Error: return statement is not in function body"
                << std::endl;
    }
    else {
      HIR::datatype_t dt = e ? e->type : HIR::Void();
      rettype_stack_.top().push_back(dt);
    }
    return HIR::Return(e);
  }

  antlrcpp::Any visitIf(AST::If_t node) override {
    HIR::expr_t test = visit(node->test);
    if (!is_boolean_type(test->type)) {
      sema_err_ << "Error: conditional must be a boolean, not "
                << to_string(test->type) << std::endl;
    }
    std::vector<HIR::stmt_t> body;
    push_scope();
    for (auto b: node->body) {
      body.push_back(visit(b));
    }
    pop_scope();
    std::vector<HIR::stmt_t> orelse;
    push_scope();
    for (auto o: node->orelse) {
      orelse.push_back(visit(o));
    }
    pop_scope();
    return HIR::If(test, body, orelse);
  }

  antlrcpp::Any visitWhile(AST::While_t node) override {
    HIR::expr_t test = visit(node->test);
    if (!is_boolean_type(test->type)) {
      sema_err_ << "Error: conditional must be a boolean, not "
                << to_string(test->type) << std::endl;
    }
    std::vector<HIR::stmt_t> body;
    push_scope();
    for (auto b: node->body) {
      body.push_back(visit(b));
    }
    pop_scope();
    return HIR::While(test, body);
  }

  antlrcpp::Any visitImport(AST::Import_t node) override {
    nyi("Import");
    return 0;
  }

  antlrcpp::Any visitImportFrom(AST::ImportFrom_t node) override {
    nyi("ImportFrom");
    return 0;
  }

  antlrcpp::Any visitDecl(AST::Decl_t node) override {
    HIR::decltype_t dt = visit(node->dt);
    std::vector<HIR::declaration_t> decls;
    for (AST::declaration_t p: node->decls) {
      decls.push_back(visit(p));
    }
    return HIR::Decl(dt, decls);
  }

  antlrcpp::Any visitAssign(AST::Assign_t node) override {
    HIR::expr_t target = visit(node->target);
    HIR::expr_t value = visit(node->value);
    if (is_temporary(target)) {
      sema_err_ << "Error: target of assignment cannot be temporary";
    }
    if (!is_same_type(target->type, value->type)) {
      sema_err_ << "Error: mismatched types in assignment: "
                << to_string(target->type) << " vs " << to_string(value->type)
                << std::endl;
    }
    if (is_void_type(value->type)) {
      sema_err_ << "Error: type 'void' is not assignable" << std::endl;
    }
    return HIR::Assign(target, value);
  }

  antlrcpp::Any visitDel(AST::Del_t node) override {
    std::vector<HIR::expr_t> target;
    for (AST::expr_t e: node->target) {
      target.push_back(visit(e));
    }
    // TODO remove target from scope
    return HIR::Del(target);
  }

  antlrcpp::Any visitExpr(AST::Expr_t node) override {
    return HIR::Expr(visit(node->value));
  }

  antlrcpp::Any visitQuery(AST::Query_t node) override {
    // determine table for query
    HIR::expr_t table = visit(node->table);
    if (!is_dataframe_type(table->type)) {
      sema_err_ << "Error: query must be on Dataframe, not "
                << to_string(table->type) << std::endl;
    }
    HIR::querytype_t qt = visit(node->qt);

    // table's scope is preferred
    preferred_scope_ = table;

    // 'by' gets its own Dataframe
    std::vector<HIR::alias_t> by;
    for (AST::alias_t b: node->by) {
      by.push_back(visit(b));
    }
    HIR::datatype_t by_type = nullptr;
    if (!by.empty()) {
      std::string ts = get_type_string(by);
      std::string by_name = anon_func_name();
      (void) create_datatype(by_name, ts);
      by_type = make_dataframe('!' + by_name);
    }

    // 'cols' change the resulting type
    std::vector<HIR::alias_t> cols;
    for (AST::alias_t c: node->cols) {
      HIR::alias_t col = visit(c);
      bool is_array = is_array_type(col->value->type);
      if (by.empty() && !is_array) {
        sema_err_ << "Error: resulting column must be an array" << std::endl;
      }
      if (!by.empty() && is_array) {
        sema_err_ << "Error: resulting column must be a scalar" << std::endl;
      }
      cols.push_back(col);
    }
    HIR::datatype_t type = table->type;
    if (!cols.empty()) {
      std::string byts = by.empty() ? "" : get_type_string(by) + ", ";
      std::string ts = byts + get_type_string(cols);
      std::string type_name = anon_func_name();
      (void) create_datatype(type_name, ts);
      type = make_dataframe('!' + type_name);
    }
    else {
      if (!by.empty()) {
        sema_err_ << "Error: must express aggregation if 'by' is listed"
                  << std::endl;
      }
    }

    // 'where' is just a boolean array
    HIR::expr_t where = nullptr;
    if (node->where) {
      where = visit(node->where);
      bool valid = false;
      if (is_array_type(where->type)) {
        HIR::Array_t arr = dynamic_cast<HIR::Array_t>(where->type);
        valid = is_boolean_type(arr->type);
      }
      if (!valid) {
        sema_err_ << "Error: 'where' must be a boolean array; got type "
                  << to_string(where->type) << std::endl;
      }
    }
    preferred_scope_ = nullptr;

    // put everything together
    return HIR::Query(table, qt, cols, by, where, by_type, type, table->name);
  }

  antlrcpp::Any visitSort(AST::Sort_t node) override {
    // determine table for query
    HIR::expr_t table = visit(node->table);
    if (!is_dataframe_type(table->type)) {
      sema_err_ << "Error: sort must be on Dataframe, not "
                << to_string(table->type) << std::endl;
    }
    HIR::datatype_t type = table->type;

    // table's scope is preferred
    preferred_scope_ = table;
    std::vector<HIR::alias_t> by;
    for (AST::alias_t b: node->by) {
      by.push_back(visit(b));
    }
    preferred_scope_ = nullptr;

    // type of 'by' items is its own Dataframe
    std::string ts = get_type_string(by);
    std::string by_name = anon_func_name();
    (void) create_datatype(by_name, ts);
    HIR::datatype_t by_type = make_dataframe('!' + by_name);

    // put everything together
    return HIR::Sort(table, by, by_type, type, table->name);
  }

  antlrcpp::Any visitJoin(AST::Join_t node) override {
    // determine tables for query
    size_t starting_err_length = sema_err_.str().size();
    HIR::expr_t left = visit(node->left);
    if (left->type != nullptr && !is_dataframe_type(left->type)) {
      sema_err_ << "Error: join for left must be on Dataframe, not "
                << to_string(left->type) << std::endl;
    }
    HIR::expr_t right = visit(node->right);
    if (right->type != nullptr && !is_dataframe_type(right->type)) {
      sema_err_ << "Error: join for right must be on Dataframe, not "
                << to_string(right->type) << std::endl;
    }
    bool bad_dfs = sema_err_.str().size() != starting_err_length;

    // determine 'on' parameters
    std::vector<HIR::alias_t> left_on;
    std::vector<HIR::alias_t> right_on;
    HIR::datatype_t left_on_type = nullptr;
    HIR::datatype_t right_on_type = nullptr;
    if (!bad_dfs && !node->on.empty()) {
      // left's scope is preferred
      preferred_scope_ = left;
      for (AST::alias_t o: node->on) {
        left_on.push_back(visit(o));
      }
      preferred_scope_ = nullptr;

      // right's scope is preferred
      preferred_scope_ = right;
      for (AST::alias_t o: node->on) {
        right_on.push_back(visit(o));
      }
      preferred_scope_ = nullptr;

      // type of 'left_on' items is its own Dataframe
      std::string left_ts = get_type_string(left_on);
      std::string left_name = anon_func_name();
      (void) create_datatype(left_name, left_ts);
      left_on_type = make_dataframe('!' + left_name);

      // type of 'right_on' items is its own Dataframe
      std::string right_ts = get_type_string(right_on);
      std::string right_name = anon_func_name();
      (void) create_datatype(right_name, right_ts);
      right_on_type = make_dataframe('!' + right_name);

      // ensure that the 'on' types are the same
      if (!is_same_type(left_on_type, right_on_type)) {
        sema_err_ << "Error: join 'on' types are not compatible: "
                  << to_string_udt(left_on_type) << " vs "
                  << to_string_udt(right_on_type) << std::endl;
      }
    }

    // determine 'asof' parameters
    HIR::alias_t left_asof = nullptr;
    HIR::alias_t right_asof = nullptr;
    HIR::datatype_t left_asof_type = nullptr;
    HIR::datatype_t right_asof_type = nullptr;
    std::string right_asof_name;
    bool strict = node->strict;
    HIR::direction_t direction = visit(node->direction);
    HIR::expr_t within = nullptr;
    if (node->within != nullptr) {
      within = visit(node->within);
    }
    if (!bad_dfs && node->asof != nullptr) {
      // left's scope is preferred
      preferred_scope_ = left;
      left_asof = visit(node->asof);
      left_asof_type = left_asof->value->type;
      preferred_scope_ = nullptr;

      // right's scope is preferred
      preferred_scope_ = right;
      right_asof = visit(node->asof);
      right_asof_type = right_asof->value->type;
      right_asof_name = right_asof->name.empty() ? right_asof->value->name
                                                 : right_asof->name;
      preferred_scope_ = nullptr;

      // ensure that the 'asof' types are the same
      if (!is_same_type(left_asof_type, right_asof_type)) {
        sema_err_ << "Error: join 'asof' types are not compatible: "
                  << to_string(left_asof_type) << " vs "
                  << to_string(right_asof_type) << std::endl;
      }

      // ensure columns allow subtraction for nearest/within
      if (within != nullptr || direction == HIR::direction_t::kNearest) {
        // find resulting type from subtracting the two 'asof' columns
        // (this logic comes from the function call visitor)
        bool subtractable = false;
        std::vector<HIR::expr_t> args({left_asof->value, right_asof->value});
        HIR::expr_t func = visit(AST::Id(std::string("-")));
        HIR::OverloadedId_t id = dynamic_cast<HIR::OverloadedId_t>(func);
        for (HIR::resolved_t ref: id->refs) {
          HIR::datatype_t func_type = get_type(ref);
          std::string result = match_args(args, func_type);
          if (result.empty()) {
            // check that subtraction's type is the same as within's
            HIR::datatype_t rettype = get_rettype(func_type);
            if (is_array_type(rettype)) {
              subtractable = true;
              if (within != nullptr) {
                HIR::Array_t arr_type = dynamic_cast<HIR::Array_t>(rettype);
                if (!is_same_type(arr_type->type, within->type)) {
                  sema_err_ << "Error: join 'asof' types not compatible "
                            << "with 'within': expected "
                            << to_string(arr_type->type) << ", got "
                            << to_string(within->type) << std::endl;
                }
              }
            }
            break;
          }
        }
        if (!subtractable) {
          sema_err_ << "Error: join 'asof' types prohibit 'within' or "
                    <<"'nearest': " << to_string(left_asof_type) << std::endl;
        }
      }

      // nearest with strict makes no sense
      if (strict && direction == HIR::direction_t::kNearest) {
        sema_err_ << "Error: join 'asof' cannot be both 'nearest' and 'strict'"
                  << std::endl;
      }
    }

    // drop right_on/right_asof from right's table type
    HIR::datatype_t remaining_type = nullptr;
    std::string remaining_ts;
    if (!bad_dfs) {
      remaining_ts = drop_columns(right->type, right_on_type, right_asof_name);
      std::string remaining_name = anon_func_name();
      (void) create_datatype(remaining_name, remaining_ts);
      remaining_type = make_dataframe('!' + remaining_name);
    }

    // combine left's type and right's remaining type
    HIR::datatype_t full_type = nullptr;
    if (!bad_dfs) {
      std::string full_ts = get_type_string(left->type) + ", " + remaining_ts;
      std::string full_name = anon_func_name();
      (void) create_datatype(full_name, full_ts);
      full_type = make_dataframe('!' + full_name);
    }

    // put everything together
    return HIR::Join(left, right, left_on, right_on, left_on_type,
                     right_on_type, left_asof, right_asof, strict, direction,
                     within, remaining_type, full_type,
                     left->name + right->name);
  }

  antlrcpp::Any visitUnaryOp(AST::UnaryOp_t node) override {
    // operator expressions are just syntactic sugar for function calls
    AST::expr_t desugar = AST::FunctionCall(AST::Id(node->op),
                                            {node->operand});
    HIR::expr_t result = visit(desugar);
    HIR::FunctionCall_t func_call = dynamic_cast<HIR::FunctionCall_t>(result);

    // repack results into sugared form
    HIR::resolved_t ref = nullptr;
    if (func_call->func->expr_kind == HIR::expr_::ExprKind::kId) {
      HIR::Id_t id = dynamic_cast<HIR::Id_t>(func_call->func);
      ref = id->ref;
    }
    HIR::expr_t operand = func_call->args[0];
    return HIR::UnaryOp(node->op, operand, ref, func_call->type,
                        func_call->name);
  }

  antlrcpp::Any visitBinOp(AST::BinOp_t node) override {
    // operator expressions are just syntactic sugar for function calls
    AST::expr_t desugar = AST::FunctionCall(AST::Id(node->op),
                                            {node->left, node->right});
    HIR::expr_t result = visit(desugar);
    HIR::FunctionCall_t func_call = dynamic_cast<HIR::FunctionCall_t>(result);

    // repack results into sugared form
    HIR::resolved_t ref = nullptr;
    if (func_call->func->expr_kind == HIR::expr_::ExprKind::kId) {
      HIR::Id_t id = dynamic_cast<HIR::Id_t>(func_call->func);
      ref = id->ref;
    }
    HIR::expr_t left = func_call->args[0];
    HIR::expr_t right = func_call->args[1];
    return HIR::BinOp(left, node->op, right, ref, func_call->type,
                      func_call->name);
  }

  antlrcpp::Any visitFunctionCall(AST::FunctionCall_t node) override {
    HIR::expr_t func = visit(node->func);
    if (!is_callable(func->type)) {
      sema_err_ << "Error: type " << to_string(func->type)
                << " is not callable" << std::endl;
    }
    std::vector<HIR::expr_t> args;
    for (AST::expr_t e: node->args) {
      args.push_back(visit(e));
    }
    // check for generic functions
    HIR::GenericFunctionDef_t generic = get_generic(func);
    if (generic != nullptr) {
      // check instantiated items first
      bool previously_instantiated = false;
      for (HIR::stmt_t instantiated: generic->instantiated_funcs) {
        HIR::FunctionDef_t def =
          dynamic_cast<HIR::FunctionDef_t>(instantiated);
        HIR::datatype_t func_type = get_type(def);
        std::string result = match_args(args, func_type);
        if (result.empty()) {
          // replace generic with previously instantiated function
          HIR::resolved_t ref = FuncRef(def);
          func = HIR::Id(def->name, ref, func_type, def->name);
          previously_instantiated = true;
          break;
        }
      }
      // create new instantiated since nothing appropriate found
      if (!previously_instantiated) {
        HIR::FunctionDef_t def =
          dynamic_cast<HIR::FunctionDef_t>(generic->original_func);
        HIR::datatype_t func_type = get_type(def);
        std::string err_msg = match_args(args, func_type);
        if (err_msg.empty()) {
          // fill nullptr using args
          std::vector<HIR::declaration_t> new_args;
          std::vector<HIR::datatype_t> argtypes = get_argtypes(func_type);
          for (size_t i = 0; i < args.size(); i++) {
            HIR::datatype_t type = argtypes[i] != nullptr ? argtypes[i]
                                                          : args[i]->type;
            new_args.push_back(HIR::declaration(def->args[i]->name, nullptr,
                                                def->args[i]->value, type, 0));
          }
          // TODO recursively copy def's body (needs HIR visitor)
          std::vector<HIR::stmt_t> new_body;
          // generate function
          HIR::stmt_t new_def = HIR::FunctionDef(def->name, new_args, new_body,
                                                 nullptr, def->docstring,
                                                 def->rettype);
          generic->instantiated_funcs.push_back(new_def);
          HIR::resolved_t ref = FuncRef(new_def);
          HIR::FunctionDef_t node = dynamic_cast<HIR::FunctionDef_t>(new_def);
          HIR::datatype_t new_func_type = get_type(node);
          func = HIR::Id(def->name, ref, new_func_type, def->name);
        }
        else {
          sema_err_ << "Error: " << err_msg << std::endl;
        }
      }
    }
    // check for overloaded functions
    // TODO we will eventually want specialization of generic functions
    // TODO handle higher-order overloaded functions
    else if (is_overloaded(func)) {
      HIR::OverloadedId_t id = dynamic_cast<HIR::OverloadedId_t>(func);
      std::string err_msg;
      size_t counted_mismatch = 0;
      const size_t max_counted = 3;
      for (HIR::resolved_t ref: id->refs) {
        HIR::datatype_t func_type = get_type(ref);
        std::string result = match_args(args, func_type);
        if (result.empty()) {
          // replace overload with specific function
          func = HIR::Id(id->s, ref, func_type, id->s);
          err_msg.clear();
          break;
        }
        else {
          counted_mismatch++;
          if (counted_mismatch <= max_counted) {
            err_msg += "\n  candidate: " + to_string(func_type) + "\n    "
                    + result;
          }
        }
      }
      if (!err_msg.empty()) {
        if (counted_mismatch > max_counted) {
          err_msg += "\n  ...\n  <"
                  + std::to_string(counted_mismatch - max_counted)
                  + " others>";
        }
        sema_err_ << "Error: unable to match overloaded function " << id->s
                  << err_msg << std::endl;
      }
    }
    // regular (non-overloaded, non-generic) function
    else {
      std::string err_msg = match_args(args, func->type);
      if (!err_msg.empty()) {
        sema_err_ << "Error: " << err_msg << std::endl;
      }
    }
    HIR::datatype_t rettype = get_rettype(func->type);
    std::string name = !args.empty() ? args[0]->name : func->name;
    return HIR::FunctionCall(func, args, rettype, name);
  }

  antlrcpp::Any visitTemplateInst(AST::TemplateInst_t node) override {
    // TODO for now value must be "load"; allow anything in future
    if (node->value->expr_kind != AST::expr_::ExprKind::kId) {
      nyi("TemplateInst on non-Id");
    }
    AST::Id_t ptr = dynamic_cast<AST::Id_t>(node->value);
    if (ptr->s != "load") {
      nyi("TemplateInst on non-load");
    }
    HIR::expr_t value = HIR::Id("load", nullptr, nullptr, "load");
    std::vector<HIR::expr_t> args;
    for (AST::expr_t e: node->args) {
      args.push_back(visit(e));
    }
    // statically evaluate arguments
    std::vector<HIR::stmt_t> resolutions;
    std::string type_name;
    for (HIR::expr_t e: args) {
      // ensure arg's type is String and then evaluate via VVM
      // TODO this needs proper CFTE because variables must be set already
      // (ie., need '$let' to force compiler to pre-set variables)
      if (is_string_type(e->type)) {
        HIR::mod_t mod = HIR::Module({HIR::Expr(e)}, "");
        VVM::Program program = codegen(mod, true, false);
        std::string filename = VVM::interpret(program);
        filename = filename.substr(1, filename.size() - 2);  // chop quotes
        std::string typestr = VVM::infer_table_from_file(filename);
        type_name = "Provider$" + filename;
        HIR::stmt_t datatype = create_datatype(type_name, typestr);
        resolutions.push_back(datatype);
      }
      else {
        sema_err_ << "Error: 'load' expects a String parameter" << std::endl;
      }
    }
    HIR::datatype_t rettype = make_dataframe('!' + type_name);
    return HIR::TemplateInst(value, args, resolutions, rettype, value->name);
  }

  antlrcpp::Any visitMember(AST::Member_t node) override {
    HIR::expr_t value = visit(node->value);
    size_t scope = get_scope(value->type);
    if (scope == 0) {
      sema_err_ << "Error: value does not have members" << std::endl;
    }
    Scope::Resolveds resolveds = find_symbol_in_scope(node->member, scope);
    if (scope != 0 && resolveds.size() == 0) {
      sema_err_ << "Error: " << node->member
                << " is not a member" << std::endl;
    }
    HIR::resolved_t ref = (resolveds.size() == 1) ? resolveds[0] : nullptr;
    HIR::datatype_t type = get_type(ref);
    if (ref != nullptr && type == nullptr) {
      sema_err_ << "Error: unable to resolve type" << std::endl;
    }
    return HIR::Member(value, node->member, ref, type, node->member);
  }

  antlrcpp::Any visitSubscript(AST::Subscript_t node) override {
    HIR::expr_t value = visit(node->value);
    if (!is_array_type(value->type)) {
      sema_err_ << "Error: value must be an array; got type "
                << to_string(value->type) << std::endl;
    }
    HIR::slice_t slice = visit(node->slice);
    // for an index (non-slice) subscript, the result is the underlying type
    HIR::datatype_t type = value->type;
    if (!is_slice(slice)) {
      type = get_underlying_type(type);
    }
    return HIR::Subscript(value, slice, type, value->name);
  }

  antlrcpp::Any visitUserDefinedLiteral(AST::UserDefinedLiteral_t node) override {
    // user-defined literals are just syntactic sugar for funcion calls
    AST::expr_t desugar = AST::FunctionCall(AST::Id("suffix" + node->suffix),
                                            {node->literal});

    HIR::expr_t result = visit(desugar);
    HIR::FunctionCall_t func_call = dynamic_cast<HIR::FunctionCall_t>(result);

    // repack results into sugared form
    HIR::resolved_t ref = nullptr;
    if (func_call->func->expr_kind == HIR::expr_::ExprKind::kId) {
      HIR::Id_t id = dynamic_cast<HIR::Id_t>(func_call->func);
      ref = id->ref;
    }
    HIR::expr_t literal = func_call->args[0];
    return HIR::UserDefinedLiteral(literal, node->suffix, ref, func_call->type,
                                   func_call->name);
  }

  antlrcpp::Any visitIntegerLiteral(AST::IntegerLiteral_t node) override {
    return HIR::IntegerLiteral(node->n, HIR::VVMType(size_t(VVM::vvm_types::i64s)), "");
  }

  antlrcpp::Any visitFloatingLiteral(AST::FloatingLiteral_t node) override {
    return HIR::FloatingLiteral(node->n, HIR::VVMType(size_t(VVM::vvm_types::f64s)), "");
  }

  antlrcpp::Any visitBoolLiteral(AST::BoolLiteral_t node) override {
    return HIR::BoolLiteral(node->b, HIR::VVMType(size_t(VVM::vvm_types::b8s)), "");
  }

  antlrcpp::Any visitStr(AST::Str_t node) override {
    return HIR::Str(node->s, HIR::VVMType(size_t(VVM::vvm_types::Ss)), "");
  }

  antlrcpp::Any visitChar(AST::Char_t node) override {
    return HIR::Char(node->c, HIR::VVMType(size_t(VVM::vvm_types::c8s)), "");
  }

  antlrcpp::Any visitId(AST::Id_t node) override {
    // Dataframes need up-front attention
    if (node->s[0] == '!') {
      (void) make_dataframe(node->s);
    }

    // look for symbol
    bool in_preferred;
    Scope::Resolveds resolveds = find_symbol(node->s, &in_preferred);
    if (resolveds.size() == 0) {
      sema_err_ << "Error: symbol " << node->s
                << " was not found" << std::endl;
    }
    if (resolveds.size() <= 1) {
      HIR::resolved_t ptr = (resolveds.size() == 1) ? resolveds[0] : nullptr;
      HIR::datatype_t type = get_type(ptr);
      if (in_preferred) {
        return HIR::ImpliedMember(node->s, ptr, preferred_scope_, type,
                                  node->s);
      }
      return HIR::Id(node->s, ptr, type, node->s);
    }
    HIR::datatype_t temp_type = get_type(resolveds[0]);
    return HIR::OverloadedId(node->s, resolveds, temp_type, node->s);
  }

  antlrcpp::Any visitList(AST::List_t node) override {
    std::vector<HIR::expr_t> values;
    for (AST::expr_t v: node->values) {
      HIR::expr_t e = visit(v);
      values.push_back(e);
    }
    // check that all types are the same
    HIR::datatype_t expected = !values.empty() ? values[0]->type : nullptr;
    for (HIR::expr_t e: values) {
      if (!is_same_type(e->type, expected)) {
        sema_err_ << "Error: mismtach in list: " << to_string(e->type)
                  << " vs " << to_string(expected) << std::endl;
      }
    }
    std::string name = !values.empty() ? values[0]->name : "";
    HIR::datatype_t type = nullptr;
    // a list of kinds means we have a kind of array
    if (is_kind_type(expected)) {
      type = HIR::Kind(HIR::Array(get_underlying_type(expected)));
      if (values.size() >= 2) {
        sema_err_ << "Error: only one type allowed for lists" << std::endl;
      }
    }
    else {
      type = HIR::Array(expected);
    }
    return HIR::List(values, type, name);
  }

  antlrcpp::Any visitParen(AST::Paren_t node) override {
    HIR::expr_t subexpr = visit(node->subexpr);
    return HIR::Paren(subexpr, subexpr->type, subexpr->name);
  }

  antlrcpp::Any visitSlice(AST::Slice_t node) override {
    HIR::expr_t lower = nullptr;
    if (node->lower) {
      lower = visit(node->lower);
    }
    if (lower != nullptr && !is_indexable_type(lower->type)) {
      sema_err_ << "Error: lower bound type " << to_string(lower->type)
                << " cannot be used as an index" << std::endl;
    }
    HIR::expr_t upper = nullptr;
    if (node->upper) {
      upper = visit(node->upper);
    }
    if (upper != nullptr && !is_indexable_type(upper->type)) {
      sema_err_ << "Error: upper bound type " << to_string(upper->type)
                << " cannot be used as an index" << std::endl;
    }
    HIR::expr_t step = nullptr;
    if (node->step) {
      step = visit(node->step);
    }
    if (step != nullptr && !is_indexable_type(step->type)) {
      sema_err_ << "Error: step type " << to_string(step->type)
                << " cannot be used as an index" << std::endl;
    }
    return HIR::Slice(lower, upper, step);
  }

  antlrcpp::Any visitIndex(AST::Index_t node) override {
    HIR::expr_t value = visit(node->value);
    if (!is_indexable_type(value->type)) {
      sema_err_ << "Error: type " << to_string(value->type)
                << " cannot be used as an index" << std::endl;
    }
    return HIR::Index(value);
  }

  antlrcpp::Any visitAlias(AST::alias_t node) override {
    if (!node->name.empty() && isupper(node->name[0])) {
      sema_err_ << "Error: value name " << node->name
                << " must begin with lower-case letter" << std::endl;
    }
    HIR::expr_t value = visit(node->value);
    return HIR::alias(value, node->name);
  }

  antlrcpp::Any visitDeclaration(AST::declaration_t node) override {
    size_t starting_err_length = sema_err_.str().size();
    if (isupper(node->name[0])) {
      sema_err_ << "Error: value name " << node->name
                << " must begin with lower-case letter" << std::endl;
    }
    // get explcit type
    HIR::expr_t explicit_type = nullptr;
    if (node->explicit_type) {
      explicit_type = visit(node->explicit_type);
    }
    HIR::datatype_t type = nullptr;
    if (explicit_type != nullptr) {
      if (is_kind_type(explicit_type->type)) {
        type = get_underlying_type(explicit_type->type);
      }
      else {
        sema_err_ << "Error: declaration for " << node->name
                  << " has invalid type" << std::endl;
      }
    }
    // get value
    HIR::expr_t value = nullptr;
    if (node->value) {
      value = visit(node->value);
    }
    if (type == nullptr && value != nullptr) {
      type = value->type;
    }
    if (value != nullptr && !is_same_type(type, value->type)) {
      sema_err_ << "Error: type of declaration does not match: "
                << to_string(type) << " vs " << to_string(value->type)
                << std::endl;
    }
    if (type == nullptr) {
      sema_err_ << "Error: unable to determine type" << std::endl;
    }
    if (is_void_type(type)) {
      sema_err_ << "Error: symbol cannot have a 'void' type" << std::endl;
    }
    // construct reference if no errors occurred so far
    HIR::declaration_t new_node = HIR::declaration(node->name, explicit_type,
                                                   value, type, 0);
    if (sema_err_.str().size() == starting_err_length) {
      if (!store_symbol(node->name, HIR::DeclRef(new_node))) {
        sema_err_ << "Error: symbol " << node->name
                  << " was already defined" << std::endl;
      }
    }
    return new_node;
  }

  antlrcpp::Any visitDecltype(AST::decltype_t value) override {
    return HIR::decltype_t(uint8_t(value));
  }

  antlrcpp::Any visitQuerytype(AST::querytype_t value) override {
    return HIR::querytype_t(uint8_t(value));
  }

  antlrcpp::Any visitDirection(AST::direction_t value) override {
    return HIR::direction_t(uint8_t(value));
  }

 public:

  SemaVisitor() {
    // start with a single global scope
    current_scope_ = 0;
    preferred_scope_ = nullptr;
    push_scope();

    // save all builtins to global scope
    save_builtins();
  }

  std::string get_errors() const {
    return sema_err_.str();
  }

  void set_interactive(bool b) {
    interactive_ = b;
  }
} sema_visitor;


// semantic analysis converts AST into HIR
HIR::mod_t sema(AST::mod_t ast, bool interactive, bool dump_hir) {
  // build high-level IR
  sema_visitor.set_interactive(interactive);
  HIR::mod_t hir = sema_visitor.visit(ast);
  std::string msg = sema_visitor.get_errors();
  if (!msg.empty()) {
    throw std::logic_error(msg);
  }

  // print high-level IR
  if (dump_hir) {
    std::cout << HIR::to_string(hir) << std::endl;
  }

  return hir;
}

