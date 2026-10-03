# nil where an attached function, or a callback's return, wants a C integer
# or double is a TypeError, as the ffi gem's NUM2INT / NUM2DBL raise. It was
# passed to C as 0 / 0.0 (nil.to_f), and the Integer one named NilClass.
require "ffi"

module LibM
  extend FFI::Library
  ffi_lib FFI::Library::LIBC
  attach_function :abs, [:int], :int
  attach_function :labs, [:long], :long
end
module LibMath
  extend FFI::Library
  ffi_lib FFI::Library::CURRENT_PROCESS
  attach_function :fabs, [:double], :double
  attach_function :fabsf, [:float], :float
end

p LibM.abs(-3)
begin
  p LibM.abs(nil)
rescue TypeError => e
  puts "TypeError: #{e.message}"
end
begin
  p LibM.labs(nil)
rescue TypeError => e
  puts "TypeError: #{e.message}"
end
p LibMath.fabs(-2.5)
begin
  p LibMath.fabs(nil)
rescue TypeError => e
  puts "TypeError: #{e.message}"
end
begin
  p LibMath.fabsf(nil)
rescue TypeError => e
  puts "TypeError: #{e.message}"
end

cb = FFI::Function.new(:int, [:int]) { |x| x > 0 ? x : nil }
p cb.call(4)
begin
  p cb.call(-1)
rescue TypeError => e
  puts "TypeError: #{e.message}"
end
fcb = FFI::Function.new(:double, [:double]) { |x| x > 0 ? x : nil }
p fcb.call(1.5)
begin
  p fcb.call(-1.0)
rescue TypeError => e
  puts "TypeError: #{e.message}"
end
