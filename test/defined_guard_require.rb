# `require X unless defined?(Const)` loads X when Const is not yet defined.
# A same-named constant of another namespace (GuardUser::GuardLib, as
# ffi-yajl's FFI_Yajl::FFI beside `require "ffi" unless defined?(FFI)`) is
# not the top-level one the guard asks about, and neither is the copy of the
# file inlined under the other arm of an if/else that requires it twice.
require_relative "defined_guard_require/solo" unless defined?(SoloLib)
module Holder
  SoloLib = :mine
end
p SoloLib.v
p Holder::SoloLib

if ENV["SPINEL_TEST_NEVER_SET_XYZ"] == "x"
  require_relative "defined_guard_require/wrap"
  puts "x"
else
  require_relative "defined_guard_require/wrap"
  puts "not x"
end
p GuardLib.get(:a)
p GuardUser::GuardLib.tag
