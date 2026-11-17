# spinel: not-cruby -- the spinel-native FFI DSL (ffi_lib / ffi_func); the .expected is authored
# ffi_lib folds its names as ffi_cflags does: String#+, adjacent literals,
# __dir__ and File.expand_path, alone or as an Array's candidates. A name
# that did not fold used to be dropped without a word, and the program then
# failed to link on an undefined symbol (or, as M1's Array, was refused for
# holding no String). test/reject/ffi_lib_runtime_name.rb is the refusal of
# a name that cannot fold.
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
p M1.sqrt(16.0)
p M2.fabs(-3.0)
p M3.floor(2.5)
