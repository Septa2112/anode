
#include "node_snapshotable.h"
#include <iostream>
#include <sstream>
#include "base_object-inl.h"
#include "debug_utils-inl.h"
#include "env-inl.h"
#include "node_blob.h"
#include "node_errors.h"
#include "node_external_reference.h"
#include "node_file.h"
#include "node_internals.h"
#include "node_main_instance.h"
#include "node_process.h"
#include "node_v8.h"
#include "node_v8_platform-inl.h"

#if HAVE_INSPECTOR
#include "inspector/worker_inspector.h"  // ParentInspectorHandle
#endif

namespace node {

using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::HandleScope;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::Object;
using v8::ScriptCompiler;
using v8::ScriptOrigin;
using v8::SnapshotCreator;
using v8::StartupData;
using v8::String;
using v8::TryCatch;
using v8::Value;

template <typename T>
void WriteVector(std::ostringstream* ss, const T* vec, size_t size) {
  for (size_t i = 0; i < size; i++) {
    *ss << std::to_string(vec[i]) << (i == size - 1 ? '\n' : ',');
  }
}

std::string FormatBlob(SnapshotData* data) {
  std::ostringstream ss;

  ss << R"(#include <cstddef>
#include "env.h"
#include "node_main_instance.h"
#include "v8.h"

// This file is generated by tools/snapshot. Do not edit.

namespace node {

static const char blob_data[] = {
)";
  WriteVector(&ss, data->blob.data, data->blob.raw_size);
  ss << R"(};

static const int blob_size = )"
     << data->blob.raw_size << R"(;

SnapshotData snapshot_data {
  // -- blob begins --
  { blob_data, blob_size },
  // -- blob ends --
  // -- isolate_data_indices begins --
  {
)";
  WriteVector(&ss,
              data->isolate_data_indices.data(),
              data->isolate_data_indices.size());
  ss << R"(},
  // -- isolate_data_indices ends --
  // -- env_info begins --
)" << data->env_info
  << R"(
  // -- env_info ends --
};

const SnapshotData* NodeMainInstance::GetEmbeddedSnapshotData() {
  return &snapshot_data;
}
}  // namespace node
)";

  return ss.str();
}

void SnapshotBuilder::Generate(SnapshotData* out,
                               const std::vector<std::string> args,
                               const std::vector<std::string> exec_args) {
  Isolate* isolate = Isolate::Allocate();
  isolate->SetCaptureStackTraceForUncaughtExceptions(
      true, 10, v8::StackTrace::StackTraceOptions::kDetailed);
  per_process::v8_platform.Platform()->RegisterIsolate(isolate,
                                                       uv_default_loop());
  std::unique_ptr<NodeMainInstance> main_instance;
  std::string result;

  {
    const std::vector<intptr_t>& external_references =
        NodeMainInstance::CollectExternalReferences();
    SnapshotCreator creator(isolate, external_references.data());
    Environment* env;
    {
      main_instance =
          NodeMainInstance::Create(isolate,
                                   uv_default_loop(),
                                   per_process::v8_platform.Platform(),
                                   args,
                                   exec_args);

      HandleScope scope(isolate);
      creator.SetDefaultContext(Context::New(isolate));
      out->isolate_data_indices =
          main_instance->isolate_data()->Serialize(&creator);

      // Run the per-context scripts
      Local<Context> context;
      {
        TryCatch bootstrapCatch(isolate);
        context = NewContext(isolate);
        if (bootstrapCatch.HasCaught()) {
          PrintCaughtException(isolate, context, bootstrapCatch);
          abort();
        }
      }
      Context::Scope context_scope(context);

      // Create the environment
      env = new Environment(main_instance->isolate_data(),
                            context,
                            args,
                            exec_args,
                            nullptr,
                            node::EnvironmentFlags::kDefaultFlags,
                            {});

      // Run scripts in lib/internal/bootstrap/
      {
        TryCatch bootstrapCatch(isolate);
        MaybeLocal<Value> result = env->RunBootstrapping();
        if (bootstrapCatch.HasCaught()) {
          PrintCaughtException(isolate, context, bootstrapCatch);
        }
        result.ToLocalChecked();
      }

      // If --build-snapshot is true, lib/internal/main/mksnapshot.js would be
      // loaded via LoadEnvironment() to execute process.argv[1] as the entry
      // point (we currently only support this kind of entry point, but we
      // could also explore snapshotting other kinds of execution modes
      // in the future).
      if (per_process::cli_options->build_snapshot) {
#if HAVE_INSPECTOR
        env->InitializeInspector({});
#endif
        TryCatch bootstrapCatch(isolate);
        // TODO(joyeecheung): we could use the result for something special,
        // like setting up initializers that should be invoked at snapshot
        // dehydration.
        MaybeLocal<Value> result =
            LoadEnvironment(env, StartExecutionCallback{});
        if (bootstrapCatch.HasCaught()) {
          PrintCaughtException(isolate, context, bootstrapCatch);
        }
        result.ToLocalChecked();
        // FIXME(joyeecheung): right now running the loop in the snapshot
        // builder seems to introduces inconsistencies in JS land that need to
        // be synchronized again after snapshot restoration.
        int exit_code = SpinEventLoop(env).FromMaybe(1);
        CHECK_EQ(exit_code, 0);
        if (bootstrapCatch.HasCaught()) {
          PrintCaughtException(isolate, context, bootstrapCatch);
          abort();
        }
      }

      if (per_process::enabled_debug_list.enabled(DebugCategory::MKSNAPSHOT)) {
        env->PrintAllBaseObjects();
        printf("Environment = %p\n", env);
      }

      // Serialize the native states
      out->env_info = env->Serialize(&creator);
      // Serialize the context
      size_t index = creator.AddContext(
          context, {SerializeNodeContextInternalFields, env});
      CHECK_EQ(index, NodeMainInstance::kNodeContextIndex);
    }

    // Must be out of HandleScope
    out->blob =
        creator.CreateBlob(SnapshotCreator::FunctionCodeHandling::kClear);

    // We must be able to rehash the blob when we restore it or otherwise
    // the hash seed would be fixed by V8, introducing a vulnerability.
    CHECK(out->blob.CanBeRehashed());

    // We cannot resurrect the handles from the snapshot, so make sure that
    // no handles are left open in the environment after the blob is created
    // (which should trigger a GC and close all handles that can be closed).
    if (!env->req_wrap_queue()->IsEmpty()
        || !env->handle_wrap_queue()->IsEmpty()
        || per_process::enabled_debug_list.enabled(DebugCategory::MKSNAPSHOT)) {
      PrintLibuvHandleInformation(env->event_loop(), stderr);
    }
    CHECK(env->req_wrap_queue()->IsEmpty());
    CHECK(env->handle_wrap_queue()->IsEmpty());

    // Must be done while the snapshot creator isolate is entered i.e. the
    // creator is still alive.
    FreeEnvironment(env);
    main_instance->Dispose();
  }

  per_process::v8_platform.Platform()->UnregisterIsolate(isolate);
}

std::string SnapshotBuilder::Generate(
    const std::vector<std::string> args,
    const std::vector<std::string> exec_args) {
  SnapshotData data;
  Generate(&data, args, exec_args);
  std::string result = FormatBlob(&data);
  delete[] data.blob.data;
  return result;
}

SnapshotableObject::SnapshotableObject(Environment* env,
                                       Local<Object> wrap,
                                       EmbedderObjectType type)
    : BaseObject(env, wrap), type_(type) {
}

const char* SnapshotableObject::GetTypeNameChars() const {
  switch (type_) {
#define V(PropertyName, NativeTypeName)                                        \
  case EmbedderObjectType::k_##PropertyName: {                                 \
    return NativeTypeName::type_name.c_str();                                  \
  }
    SERIALIZABLE_OBJECT_TYPES(V)
#undef V
    default: { UNREACHABLE(); }
  }
}

bool IsSnapshotableType(FastStringKey key) {
#define V(PropertyName, NativeTypeName)                                        \
  if (key == NativeTypeName::type_name) {                                      \
    return true;                                                               \
  }
  SERIALIZABLE_OBJECT_TYPES(V)
#undef V

  return false;
}

void DeserializeNodeInternalFields(Local<Object> holder,
                                   int index,
                                   StartupData payload,
                                   void* env) {
  per_process::Debug(DebugCategory::MKSNAPSHOT,
                     "Deserialize internal field %d of %p, size=%d\n",
                     static_cast<int>(index),
                     (*holder),
                     static_cast<int>(payload.raw_size));
  if (payload.raw_size == 0) {
    holder->SetAlignedPointerInInternalField(index, nullptr);
    return;
  }

  Environment* env_ptr = static_cast<Environment*>(env);
  const InternalFieldInfo* info =
      reinterpret_cast<const InternalFieldInfo*>(payload.data);

  switch (info->type) {
#define V(PropertyName, NativeTypeName)                                        \
  case EmbedderObjectType::k_##PropertyName: {                                 \
    per_process::Debug(DebugCategory::MKSNAPSHOT,                              \
                       "Object %p is %s\n",                                    \
                       (*holder),                                              \
                       NativeTypeName::type_name.c_str());                     \
    env_ptr->EnqueueDeserializeRequest(                                        \
        NativeTypeName::Deserialize, holder, index, info->Copy());             \
    break;                                                                     \
  }
    SERIALIZABLE_OBJECT_TYPES(V)
#undef V
    default: { UNREACHABLE(); }
  }
}

StartupData SerializeNodeContextInternalFields(Local<Object> holder,
                                               int index,
                                               void* env) {
  per_process::Debug(DebugCategory::MKSNAPSHOT,
                     "Serialize internal field, index=%d, holder=%p\n",
                     static_cast<int>(index),
                     *holder);
  void* ptr = holder->GetAlignedPointerFromInternalField(BaseObject::kSlot);
  if (ptr == nullptr) {
    return StartupData{nullptr, 0};
  }

  DCHECK(static_cast<BaseObject*>(ptr)->is_snapshotable());
  SnapshotableObject* obj = static_cast<SnapshotableObject*>(ptr);
  per_process::Debug(DebugCategory::MKSNAPSHOT,
                     "Object %p is %s, ",
                     *holder,
                     obj->GetTypeNameChars());
  InternalFieldInfo* info = obj->Serialize(index);
  per_process::Debug(DebugCategory::MKSNAPSHOT,
                     "payload size=%d\n",
                     static_cast<int>(info->length));
  return StartupData{reinterpret_cast<const char*>(info),
                     static_cast<int>(info->length)};
}

void SerializeBindingData(Environment* env,
                          SnapshotCreator* creator,
                          EnvSerializeInfo* info) {
  size_t i = 0;
  env->ForEachBindingData([&](FastStringKey key,
                              BaseObjectPtr<BaseObject> binding) {
    per_process::Debug(DebugCategory::MKSNAPSHOT,
                       "Serialize binding %i, %p, type=%s\n",
                       static_cast<int>(i),
                       *(binding->object()),
                       key.c_str());

    if (IsSnapshotableType(key)) {
      size_t index = creator->AddData(env->context(), binding->object());
      per_process::Debug(DebugCategory::MKSNAPSHOT,
                         "Serialized with index=%d\n",
                         static_cast<int>(index));
      info->bindings.push_back({key.c_str(), i, index});
      SnapshotableObject* ptr = static_cast<SnapshotableObject*>(binding.get());
      ptr->PrepareForSerialization(env->context(), creator);
    } else {
      UNREACHABLE();
    }

    i++;
  });
}

namespace mksnapshot {

static void CompileSnapshotMain(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsString());
  Local<String> filename = args[0].As<String>();
  Local<String> source = args[1].As<String>();
  Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  ScriptOrigin origin(isolate, filename, 0, 0, true);
  // TODO(joyeecheung): do we need all of these? Maybe we would want a less
  // internal version of them.
  std::vector<Local<String>> parameters = {
      FIXED_ONE_BYTE_STRING(isolate, "require"),
      FIXED_ONE_BYTE_STRING(isolate, "__filename"),
      FIXED_ONE_BYTE_STRING(isolate, "__dirname"),
  };
  ScriptCompiler::Source script_source(source, origin);
  Local<Function> fn;
  if (ScriptCompiler::CompileFunctionInContext(context,
                                               &script_source,
                                               parameters.size(),
                                               parameters.data(),
                                               0,
                                               nullptr,
                                               ScriptCompiler::kEagerCompile)
          .ToLocal(&fn)) {
    args.GetReturnValue().Set(fn);
  }
}

static void Initialize(Local<Object> target,
                       Local<Value> unused,
                       Local<Context> context,
                       void* priv) {
  Environment* env = Environment::GetCurrent(context);
  Isolate* isolate = context->GetIsolate();
  env->SetMethod(target, "compileSnapshotMain", CompileSnapshotMain);
  target
      ->Set(context,
            FIXED_ONE_BYTE_STRING(isolate, "cleanups"),
            v8::Array::New(isolate))
      .Check();
}

static void RegisterExternalReferences(ExternalReferenceRegistry* registry) {
  registry->Register(CompileSnapshotMain);
  registry->Register(MarkBootstrapComplete);
}
}  // namespace mksnapshot
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_INTERNAL(mksnapshot, node::mksnapshot::Initialize)
NODE_MODULE_EXTERNAL_REFERENCE(mksnapshot,
                               node::mksnapshot::RegisterExternalReferences)
