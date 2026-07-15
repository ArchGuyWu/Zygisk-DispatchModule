#include <android/log.h>
#include <jni.h>
#include <string.h>
#include <thread>

#include "zygisk/zygisk.hpp"

extern "C" void dispatch_bootstrap_rust(void);

namespace {

constexpr const char* kLogTag = "DispatchZygisk";
constexpr const char* kTargetPackage = "com.rockstargames.gtasa.de";
constexpr const char* kTargetPackageAlt = "com.netflix.NGP.GTASanAndreasDefinitiveEdition";

void hook_thread_func() {
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "hook thread: enter");
    dispatch_bootstrap_rust();
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "hook thread: exit");
}

}  // namespace

extern "C" void dispatch_android_log(int priority, const char* msg) {
    __android_log_write(priority, kLogTag, msg);
}

class DispatchModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        if (!args || !args->nice_name) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        const char* process = env_->GetStringUTFChars(args->nice_name, nullptr);
        if (!process) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        if (strstr(process, kTargetPackage) != nullptr ||
            strstr(process, kTargetPackageAlt) != nullptr) {
            is_target_ = true;
            __android_log_print(ANDROID_LOG_INFO, kLogTag, "target matched: %s", process);
        } else {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }

        env_->ReleaseStringUTFChars(args->nice_name, process);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs* /*args*/) override {
        if (!is_target_) {
            return;
        }
        std::thread(hook_thread_func).detach();
    }

private:
    zygisk::Api* api_ = nullptr;
    JNIEnv* env_ = nullptr;
    bool is_target_ = false;
};

extern "C" __attribute__((visibility("default"))) void zygisk_entry_rust(
    zygisk::internal::api_table* table, JNIEnv* env) {
    zygisk::internal::entry_impl<DispatchModule>(table, env);
}