# spinel: not-cruby -- ffi_callback and ffi_source are Spinel's own
# A :str argument of an ffi_callback reaches the method as a String: a copy
# of the bytes C passed, which the method may change, and nil for NULL. The
# trampoline cast the pointer to sp_int for a String handle parameter, which
# the C compiler rejected, and a boxed parameter read the marks a String's
# bytes carry from before C's own bytes.
module Raw
  ffi_source <<~'C'
    typedef void (*cb_t)(const char *s);
    static cb_t g;
    static char buf[16] = "hello";
    void reg(cb_t f) { g = f; }
    void fire(void) { g(buf); }
    void fire_null(void) { g(0); }
    const char *peek(void) { return buf; }
  C
  ffi_callback :cb_t, [:str], :void
  ffi_func :reg, [:cb_t], :void
  ffi_func :fire, [], :void
  ffi_func :fire_null, [], :void
  ffi_func :peek, [], :str
end

def on_str(s) = puts(s.upcase)

def on_change(s)
  s << "!"
  puts s
end

def on_maybe(s) = p(s)

def on_any(s)
  p s
  p s.class
end

Raw.reg(method(:on_str))
Raw.fire
Raw.reg(method(:on_change))
Raw.fire
puts Raw.peek
Raw.reg(method(:on_maybe))
Raw.fire
Raw.fire_null
on_any(1)
Raw.reg(method(:on_any))
Raw.fire
