# spinel: gc-stress
# spinel: not-cruby -- ffi_callback and ffi_source are Spinel's own
# A :str argument of an ffi_callback reaches the method as a String: a copy
# of the bytes C passed, which the method may change, and nil for NULL. The
# trampoline cast the pointer to sp_int for a String handle parameter, which
# the C compiler rejected, and a boxed parameter read the marks a String's
# bytes carry from before C's own bytes. Each copy is rooted before the next
# argument's copy is made: two in one call expression, the second's
# allocation freed the first under a collection (gc-stress).
module Raw
  ffi_source <<~'C'
    typedef void (*cb_t)(const char *s);
    typedef void (*cb2_t)(const char *a, const char *b);
    static cb_t g;
    static cb2_t g2;
    static char buf[16] = "hello";
    void reg(cb_t f) { g = f; }
    void fire(void) { g(buf); }
    void fire_null(void) { g(0); }
    const char *peek(void) { return buf; }
    void reg2(cb2_t f) { g2 = f; }
    void fire2(void) { g2(buf, "second string"); }
  C
  ffi_callback :cb_t, [:str], :void
  ffi_callback :cb2_t, [:str, :str], :void
  ffi_func :reg, [:cb_t], :void
  ffi_func :fire, [], :void
  ffi_func :fire_null, [], :void
  ffi_func :peek, [], :str
  ffi_func :reg2, [:cb2_t], :void
  ffi_func :fire2, [], :void
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

def on_two(a, b) = puts(a.upcase + " / " + b.upcase)

def on_two_any(a, b) = p([a, b])

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
Raw.reg2(method(:on_two))
3.times { Raw.fire2 }
on_two_any(1, 2)
Raw.reg2(method(:on_two_any))
3.times { Raw.fire2 }
