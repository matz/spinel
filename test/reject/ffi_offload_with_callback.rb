# An ffi_func declared `offload: true` runs on another OS thread, where an
# ffi_callback cannot run Ruby; a program with a callback is refused.
module Cb
  ffi_source <<~C
    typedef int (*cb_t)(int);
    int cb_call(cb_t f, int x) { return f(x); }
    int cb_twice(int x) { return x * 2; }
  C
  ffi_callback :cb_t, [:int], :int
  ffi_func :cb_call, [:cb_t, :int], :int
  ffi_func :cb_twice, [:int], :int, offload: true
end
p Cb.cb_twice(2)
