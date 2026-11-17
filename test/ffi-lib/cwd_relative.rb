# A one-argument File.expand_path of a relative name is relative to the
# working directory, as in CRuby; for an ffi_lib name that is the compile's,
# where the link runs. reject-test compiles this with -c from the repository
# root and checks the link marker names that directory's file. Kept relative,
# the name linked as `-l:libspinel_cwd_probe.so`, searched on the library
# path instead of read from the directory. (Compiled with -c only: no such
# library exists.)
module Probe
  ffi_lib File.expand_path("libspinel_cwd_probe.so")
  ffi_func :probe, [], :int
end
p Probe.probe
