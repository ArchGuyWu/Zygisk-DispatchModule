use std::env;
use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let cpp = manifest.join("cpp");

    for file in [
        "zygisk_entry.cpp",
        "bionic_stubs.cpp",
        "exports.lds",
        "zygisk/zygisk.hpp",
    ] {
        println!("cargo:rerun-if-changed={}", cpp.join(file).display());
    }

    let mut build = cc::Build::new();
    build
        .cpp(true)
        .std("c++17")
        .flag("-fvisibility=hidden")
        .flag("-fvisibility-inlines-hidden")
        .flag("-stdlib=libc++")
        .cpp_link_stdlib("c++_static")
        .cpp_link_stdlib_static(true)
        .file(cpp.join("zygisk_entry.cpp"))
        .file(cpp.join("bionic_stubs.cpp"))
        .include(&cpp)
        .include(cpp.join("zygisk"));
    build.compile("zygisk_entry");

    let ndk = env::var("ANDROID_NDK_HOME").unwrap_or_else(|_| "/opt/toolchain/android-ndk-r29".to_string());
    let api = env::var("ANDROID_API").unwrap_or_else(|_| "28".to_string());
    let prebuilt = format!(
        "{}/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android",
        ndk
    );
    let api_lib = format!("{prebuilt}/{api}");
    println!("cargo:rustc-link-search=native={api_lib}");
    println!("cargo:rustc-link-search=native={prebuilt}");
    println!("cargo:rustc-link-lib=static=c++abi");
    println!("cargo:rustc-link-lib=dylib=log");
}