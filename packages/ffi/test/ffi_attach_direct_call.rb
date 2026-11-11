# A literal attach_function of scalar types is called through the pointer
# the attach resolved (#11362): on the module's constant, on the module as a
# value, before its attach has run (NoMethodError), with the gem's rename
# form, and beside an attach that keeps the generic route (a :pointer).
require "ffi"

module LibA
  extend FFI::Library
  ffi_lib FFI::Library::LIBC
  begin
    LibA.labs(-1)
  rescue NoMethodError => e
    puts "before attach: #{e.class}"
  end
  attach_function :strlen, [:string], :size_t
  attach_function :abs, [:int], :int
  attach_function :labs, [:long], :long
  attach_function :c_atoi, :atoi, [:string], :int
  attach_function :atof, [:string], :double
  attach_function :getenv, [:string], :pointer
end

p LibA.strlen("hello, world")
p LibA.abs(-7), LibA.labs(-(2**40)), LibA.c_atoi("123x"), LibA.atof("-2.5")
m = LibA
p m.strlen("abc"), m.abs(-3)
p LibA.respond_to?(:strlen), LibA.getenv("SPINEL_NO_SUCH_VARIABLE_11362").null?
s = +"grow"
10.times { s << "!" }
p LibA.strlen(s)
n = 0
1000.times { |i| n += LibA.abs(-i) }
p n
