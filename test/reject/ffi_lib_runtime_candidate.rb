# Every candidate of an ffi_lib Array folds at compile time, the ones after
# a library this machine has too: one read from the environment is refused
# whether or not libm.so.6 is installed, so what a declaration may say does
# not depend on the machine that compiles it. It used to compile wherever
# the first candidate was found. test/reject/ffi_lib_runtime_name.rb is the
# same refusal for a name alone.
module Lib
  ffi_lib ["libm.so.6", ENV.fetch("SPINEL_TEST_LIB", "m")]
  ffi_func :cos, [:double], :double
end
p Lib.cos(0.0)
