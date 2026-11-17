# spinel: not-cruby -- the spinel-native FFI DSL (ffi_lib / ffi_func); the .expected is authored
# ffi_lib folds its names as ffi_cflags does: String#+, adjacent literals,
# __dir__ and File.expand_path, alone or as an Array's candidates. A name
# that did not fold used to be dropped without a word, and the program then
# failed to link on an undefined symbol (or, as M1's Array, was refused for
# holding no String). An Array of the ffi gem's constants only (M4) adds
# nothing, as one constant does. test/reject/ffi_lib_runtime_name.rb and
# ffi_lib_runtime_candidate.rb are the refusals of a name that cannot fold,
# and test/ffi-lib/cwd_relative.rb (reject-test) a one-argument
# File.expand_path, which names the working directory.
module M1
  ffi_lib ["lib" + "m.so.6"]
  ffi_func :sqrt, [:double], :double
end
module M2
  ffi_lib "lib" "m.so.6"
  ffi_func :fabs, [:double], :double
end
module M3
  ffi_lib [File.expand_path("libnosuchlib.so.9", __dir__), "m"]
  ffi_func :floor, [:double], :double
end
module M4
  ffi_lib [FFI::Library::LIBC]
  ffi_func :abs, [:int], :int
end
p M1.sqrt(16.0)
p M2.fabs(-3.0)
p M3.floor(2.5)
p M4.abs(-3)
