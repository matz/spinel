# attach_function ..., blocking: true takes the option: the function's type
# says blocking, and its calls run out of the world (a collection another
# thread starts does not wait for them), as an ffi_func declared blocking
# does (#11350)
require "ffi"
module LibC
  extend FFI::Library
  ffi_lib FFI::Library::LIBC
  attach_function :usleep, [:uint], :int, blocking: true
  attach_function :strlen, [:string], :size_t
  attach_function :abs, [:int], :int, blocking: true
end
p FFI::FunctionType.new(:int, [:uint], blocking: true).blocking
p FFI::FunctionType.new(:int, [:uint]).blocking
t = Thread.new { LibC.usleep(200_000) }
a = nil
3.times { GC.start; 2_000.times { |i| a = Array.new(5) { |j| "s#{i}-#{j}" } } }
t.join
p :joined
p LibC.abs(-7)
p LibC.strlen("hello")
p a.size
