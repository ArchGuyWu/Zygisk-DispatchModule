use std::env;
use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let workspace = manifest.join("../..");
    let xdl = workspace.join("../third_party/shadowhook-src/shadowhook/src/main/cpp/third_party/xdl");

    println!("cargo:rerun-if-changed={}", xdl.display());

    let mut build = cc::Build::new();
    build
        .std("c11")
        .include(&xdl)
        .file(xdl.join("xdl.c"))
        .file(xdl.join("xdl_iterate.c"))
        .file(xdl.join("xdl_linker.c"))
        .file(xdl.join("xdl_lzma.c"))
        .file(xdl.join("xdl_util.c"));

    build.compile("xdl");
}