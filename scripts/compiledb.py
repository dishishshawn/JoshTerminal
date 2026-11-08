# Makes compile_commands.json name the compiler by absolute path, so clangd's --query-driver can
# run it to find the toolchain headers. (The -I paths this also adds are stripped in .clangd.)
# Import/env are injected by PlatformIO's SCons runtime.
Import("env")  # type: ignore # noqa: F821
env.Replace(COMPILATIONDB_INCLUDE_TOOLCHAIN=True)  # type: ignore # noqa: F821
