/*
 * Copyright © 2019 Paweł Dziepak
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include <filesystem>
#include <sstream>
#include <string>

#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <fmt/format.h>
#include <fmt/ostream.h>

namespace codegen {

class compiler;
class module;

template<typename ReturnType, typename... Arguments> class function_ref {
  std::string name_;
  llvm::Function* function_;

public:
  explicit function_ref(std::string const& name, llvm::Function* fn) : name_(name), function_(fn) {}

  std::string const& name() const { return name_; }
};

class module_builder {
  compiler* compiler_;

public: // FIXME: proper encapsulation
  std::unique_ptr<llvm::LLVMContext> context_;
  std::unique_ptr<llvm::Module> module_;

  llvm::IRBuilder<> ir_builder_;

  class source_code_generator {
    std::stringstream source_code_;
    unsigned line_no_ = 1;
    unsigned indent_ = 0;

  public:
    unsigned add_line(std::string const&);
    void enter_scope() { indent_ += 4; }
    void leave_scope() { indent_ -= 4; }
    unsigned current_line() const { return line_no_; }
    std::string get() const;
  };
  source_code_generator source_code_;
  std::filesystem::path source_file_;

  llvm::DIBuilder dbg_builder_;

  llvm::DIFile* dbg_file_;
  llvm::DIScope* dbg_scope_;

public:
  module_builder(compiler&, std::string const& name);

  module_builder(module_builder const&) = delete;
  module_builder(module_builder&&) = delete;

  template<typename FunctionType, typename FunctionBuilder>
  auto create_function(std::string const& name, FunctionBuilder&& fb);

  [[nodiscard]] module build() &&;

  friend std::ostream& operator<<(std::ostream&, module_builder const&);
};

namespace detail {

inline thread_local module_builder* current_builder;

template<typename Type> struct type {
  static llvm::DIType* dbg();
  static llvm::Type* llvm();
  static std::string name();
};
template<> struct type<void> {
  static constexpr size_t alignment = 0;
  static llvm::DIType* dbg() { return nullptr; }
  static llvm::Type* llvm() { return llvm::Type::getVoidTy(*current_builder->context_); }
  static std::string name() { return "void"; }
};
template<> struct type<int32_t> {
  static constexpr size_t alignment = alignof(int32_t);
  static llvm::DIType* dbg() {
    return current_builder->dbg_builder_.createBasicType(name(), 32, llvm::dwarf::DW_ATE_signed);
  }
  static llvm::Type* llvm() { return llvm::Type::getInt32Ty(*current_builder->context_); }
  static std::string name() { return "i32"; }
};

template<typename Type> llvm::Value* get_constant(Type);

template<> inline llvm::Value* get_constant<int32_t>(int32_t v) {
  return llvm::ConstantInt::get(*current_builder->context_, llvm::APInt(32, v, true));
}

} // namespace detail

template<typename Type> class value {
  llvm::Value* value_;
  std::string name_;

public:
  explicit value(llvm::Value* v, std::string const& n) : value_(v), name_(n) {}

  using value_type = Type;

  operator llvm::Value*() const noexcept { return value_; }

  friend std::ostream& operator<<(std::ostream& os, value v) { return os << v.name_; }
};

template<typename Type> value<Type> constant(Type v) {
  return value<Type>{detail::get_constant<Type>(v), std::to_string(v)};
}

namespace detail {

template<typename Type> llvm::Value* eval(value<Type> v) {
  return v;
}

} // namespace detail

void return_();

template<typename Value> void return_(Value v) {
  auto& mb = *detail::current_builder;
  auto line_no = mb.source_code_.add_line(fmt::format("return {};", v));
  mb.ir_builder_.SetCurrentDebugLocation(llvm::DebugLoc::get(line_no, 1, mb.dbg_scope_));
  mb.ir_builder_.CreateRet(detail::eval(v));
}

namespace detail {

template<typename> class function_builder;

template<typename ReturnType, typename... Arguments> class function_builder<ReturnType(Arguments...)> {
  template<typename Argument> void prepare_argument(llvm::Function::arg_iterator args, size_t idx) {
    auto it = args + idx;
    auto name = "arg" + std::to_string(idx);
    it->setName(name);
  }

  template<size_t... Idx, typename FunctionBuilder>
  void call_builder(std::index_sequence<Idx...>, std::string const& name, FunctionBuilder&& fb,
                    llvm::Function::arg_iterator args) {
    auto& mb = *current_builder;

    auto str = std::stringstream{};
    str << type<ReturnType>::name() << " " << name << "(";
    (void)(str << ...
               << (type<Arguments>::name() + " arg" + std::to_string(Idx) + (Idx + 1 == sizeof...(Idx) ? "" : ", ")));
    str << ") {";
    mb.source_code_.add_line(str.str());
    mb.source_code_.enter_scope();

    [[maybe_unused]] auto _ = {0, (prepare_argument<Arguments>(args, Idx), 0)...};
    fb(value<Arguments>(&*(args + Idx), "arg" + std::to_string(Idx))...);

    mb.source_code_.leave_scope();
    mb.source_code_.add_line("}");
  }

public:
  template<typename FunctionBuilder>
  function_ref<ReturnType, Arguments...> operator()(std::string const& name, FunctionBuilder&& fb) {
    auto& mb = *current_builder;
    auto fn_type = llvm::FunctionType::get(type<ReturnType>::llvm(), {type<Arguments>::llvm()...}, false);
    auto fn = llvm::Function::Create(fn_type, llvm::GlobalValue::LinkageTypes::ExternalLinkage, name, mb.module_.get());

    std::vector<llvm::Metadata*> dbg_types = {detail::type<ReturnType>::dbg(), detail::type<Arguments>::dbg()...};
    auto dbg_fn_type = mb.dbg_builder_.createSubroutineType(mb.dbg_builder_.getOrCreateTypeArray(dbg_types));
    auto dbg_fn_scope = mb.dbg_builder_.createFunction(
        mb.dbg_scope_, name, name, mb.dbg_file_, mb.source_code_.current_line(), dbg_fn_type,
        mb.source_code_.current_line(), llvm::DINode::FlagPrototyped,
        llvm::DISubprogram::DISPFlags::SPFlagDefinition | llvm::DISubprogram::DISPFlags::SPFlagOptimized);
    auto parent_scope = std::exchange(mb.dbg_scope_, dbg_fn_scope);
    fn->setSubprogram(dbg_fn_scope);

    mb.ir_builder_.SetCurrentDebugLocation(llvm::DebugLoc{});

    auto block = llvm::BasicBlock::Create(*mb.context_, "entry", fn);
    mb.ir_builder_.SetInsertPoint(block);

    call_builder(std::index_sequence_for<Arguments...>{}, name, fb, fn->arg_begin());

    mb.dbg_scope_ = parent_scope;

    return function_ref<ReturnType, Arguments...>{name, fn};
  }
};

} // namespace detail

template<typename FunctionType, typename FunctionBuilder>
auto module_builder::create_function(std::string const& name, FunctionBuilder&& fb) {
  assert(detail::current_builder == this || !detail::current_builder);
  auto prev_builder = std::exchange(detail::current_builder, this);
  auto fn_ref = detail::function_builder<FunctionType>{}(name, fb);
  detail::current_builder = prev_builder;
  return fn_ref;
}

} // namespace codegen
