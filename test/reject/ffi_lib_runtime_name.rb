# A library name ffi_lib cannot fold at compile time (here one read from the
# environment) is refused by name; it used to be dropped without a word, and
# the program then failed to link on an undefined symbol.
# test/ffi_lib_fold_forms.rb has the names that fold.
module Lib
  ffi_lib ENV.fetch("SPINEL_TEST_LIB", "m")
  ffi_func :cos, [:double], :double
end
p Lib.cos(0.0)
