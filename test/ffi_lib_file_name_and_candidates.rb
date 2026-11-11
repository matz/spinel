# spinel: not-cruby -- the spinel-native FFI DSL (ffi_lib / ffi_func); the .expected is authored
# ffi_lib takes a library file name (a soname: `-l:<file>` for GNU ld and
# lld, the base name's `-l` on macOS), a path, and an Array of candidates,
# the first this machine has (#11363)
module M1
  ffi_lib "libm.so.6"
  ffi_func :sqrt, [:double], :double
end
module M2
  ffi_lib ["libnosuchlib.so.9", "m"]
  ffi_func :fabs, [:double], :double
end
module M3
  ffi_lib "m"
  ffi_func :floor, [:double], :double
end
p M1.sqrt(16.0)
p M2.fabs(-3.0)
p M3.floor(2.5)
