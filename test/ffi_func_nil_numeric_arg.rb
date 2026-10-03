# nil where an ffi_func argument or an ffi_callback's return is a C integer
# or double is a TypeError, as the ffi gem's NUM2INT / NUM2DBL raise. A
# boxed nil passed the 0 under its tag, a nullable Integer or Float slot
# passed its nil sentinel as a number, and a callback answering nil handed
# C the sentinel, which came back as nil. dpos is called once from Ruby so
# its parameter is a Float.
module N
  ffi_source <<~'C'
    long sp_probe_twice(long x) { return 2 * x; }
    double sp_probe_half(double x) { return x / 2; }
    typedef long (*sp_probe_cb)(long);
    long sp_probe_apply(sp_probe_cb f, long x) { return f(x); }
    typedef double (*sp_probe_fcb)(double);
    double sp_probe_fapply(sp_probe_fcb f, double x) { return f(x); }
  C
  ffi_callback :lcb, [:long], :long
  ffi_callback :dcb, [:double], :double
  ffi_func :sp_probe_twice, [:long], :long
  ffi_func :sp_probe_half, [:double], :double
  ffi_func :sp_probe_apply, [:lcb, :long], :long
  ffi_func :sp_probe_fapply, [:dcb, :double], :double
end

def show(v)
  p v
end

def try
  yield
rescue TypeError => e
  puts "TypeError: #{e.message}"
end

xs = [3, nil, "s"]
try { p N.sp_probe_twice(xs[0]) }
try { p N.sp_probe_twice(xs[1]) }
try { p N.sp_probe_twice(show(nil)) }
try { p N.sp_probe_twice(show(4)) }
fs = [3.0, nil, "s"]
try { p N.sp_probe_half(fs[0]) }
try { p N.sp_probe_half(fs[1]) }
try { p N.sp_probe_half(show(nil)) }
try { p N.sp_probe_half(show(5.0)) }

def lpos(x) = x > 0 ? x : nil
def dpos(x) = x > 0 ? x : nil
def lnil(x) = nil
p dpos(2.0)
try { p N.sp_probe_apply(method(:lpos), 7) }
try { p N.sp_probe_apply(method(:lpos), -7) }
try { p N.sp_probe_fapply(method(:dpos), 1.5) }
try { p N.sp_probe_fapply(method(:dpos), -1.5) }
try { p N.sp_probe_apply(method(:lnil), 1) }
try { p N.sp_probe_twice(nil) }
try { p N.sp_probe_half(nil) }
