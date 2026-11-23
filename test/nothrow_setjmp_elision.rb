# Whole-program conservative nothrow / may_raise analysis elides setjmp and
# volatile qualifiers on begin..rescue/ensure and rescue modifiers whose body
# provably cannot raise or non-locally unwind, while keeping handlers whenever
# an exception or non-local unwind can occur.

def leaf_mask(x)
  x & 0xff
end

def leaf_float_mix(a, b)
  (a + b) * 0.5
end

def mid_mask(x)
  y = leaf_mask(x)
  y == 0 ? 1 : y
end

class Box
  attr_reader :val

  def initialize(v)
    @val = v
  end

  def masked
    leaf_mask(@val)
  end
end

# 1. Pure begin..rescue..else..ensure calling transitive nothrow helpers:
#    setjmp and volatile elided; body -> else -> ensure run in CRuby order.
def pure_rescue_else_ensure(x)
  log = 0
  v = 0
  res = begin
    v = mid_mask(x)
    log = v
    v
  rescue StandardError
    log = -1
    -99
  else
    log = log | 0x100
    v | 0x200
  ensure
    log = log | 0x400
  end
  [res, log]
end

# 2. Method-level rescue whose body is provably nothrow: no setjmp or volatile.
def pure_def_rescue(a, b)
  x = leaf_float_mix(a, b)
  x > 0.0 ? x : -x
rescue StandardError
  -1.0
end

# 3. Pure rescue modifier in value and statement positions: no setjmp.
def pure_rescue_mod(x)
  v = (leaf_mask(x) rescue -1)
  (v = v | 0x10) rescue nil
  v
end

# 4. Pure begin..ensure with early local `return`: setjmp elided, ensure still runs.
def pure_ensure_local_return(x, box)
  flag = 0
  begin
    return leaf_mask(x) | 0x100 if x > 10
    flag = 1
    box.masked
  ensure
    flag = flag | 2
    puts "local_ensure=#{flag}"
  end
end

# 5. Pure begin..rescue inside a break-carrying block: no setjmp, no sp_brk_push.
def pure_break_with_begin(arr)
  found = -1
  arr.each do |x|
    v = begin
      leaf_mask(x)
    rescue StandardError
      -1
    end
    if v == 7
      found = v
      break found
    end
  end
  found
end

# 6. Pure yielder called only with a nothrow block: setjmp elided.
def pure_yield_wrap(x)
  begin
    yield leaf_mask(x)
  rescue StandardError
    -1
  end
end

# 7. Non-local return via Proc inside begin..ensure: setjmp MUST stay, ensure runs.
def nonlocal_proc_return_ensure
  p = proc { return 42 }
  begin
    p.call
    99
  ensure
    puts "nonlocal_proc_ensure"
  end
end

# 8. Non-local return via block yield inside begin..ensure: ensure MUST run.
def yield_with_ensure
  begin
    yield
    :fell_through
  ensure
    puts "yield_ensure_ran"
  end
end

def caller_nonlocal_yield_return
  yield_with_ensure { return 77 }
end

# 9. Transitive raising callee inside begin..rescue: setjmp MUST stay.
def deep_raise(flag)
  raise ArgumentError, "boom" if flag
  123
end

def call_deep_raise(flag)
  deep_raise(flag)
end

def caught_transitive_raise(flag)
  begin
    call_deep_raise(flag)
  rescue ArgumentError => e
    e.message
  end
end

# 10. ZeroDivisionError: variable divisor keeps setjmp; safe literal divisor elides.
def div_may_raise(a, b)
  begin
    a / b
  rescue ZeroDivisionError
    -1
  end
end

def div_safe_literal(a)
  begin
    a / 2
  rescue ZeroDivisionError
    -1
  end
end

def mod_float_may_raise(a, b)
  begin
    a % b
  rescue ZeroDivisionError
    -1.0
  end
end

# 11. TypeError on boxed operand / unboxing: setjmp MUST stay.
def type_error_unbox(x)
  begin
    1 + x
  rescue TypeError
    -2
  end
end

# 12. NoMethodError on nullable receiver and boxed receiver: setjmp MUST stay.
def nomethod_nil_recv(flag)
  b = flag ? Box.new(5) : nil
  begin
    b.masked
  rescue NoMethodError
    -3
  end
end

def nomethod_boxed_recv(x)
  begin
    x.masked
  rescue NoMethodError
    -4
  end
end

# 13. FrozenError on ivar write: setjmp MUST stay.
class MutableBox
  def initialize(v)
    @v = v
  end

  def set_v(v)
    @v = v
  end
end

def frozen_ivar_write(mb)
  begin
    mb.set_v(9)
  rescue FrozenError
    -5
  end
end

# 14. Yielder whose block can raise: setjmp MUST stay.
def raising_yield_wrap(x)
  begin
    yield x
  rescue RuntimeError => e
    e.message
  end
end

# 15. Undef'd method on a class or subclass raises NoMethodError: setjmp MUST stay.
class UndefHost
  def gone
    1
  end
  undef gone
end

def nomethod_undef_method(u)
  begin
    u.gone
  rescue NoMethodError
    -6
  end
end

# Exercise everything
b = Box.new(0x123)
p pure_rescue_else_ensure(0x105)
p pure_def_rescue(3.0, 5.0)
p pure_def_rescue(-3.0, -5.0)
p pure_rescue_mod(0x105)
p pure_ensure_local_return(20, b)
p pure_ensure_local_return(5, b)
p pure_break_with_begin([1, 2, 0x107, 9])
p pure_yield_wrap(0x109) { |v| v | 0x20 }

p nonlocal_proc_return_ensure
p caller_nonlocal_yield_return

p caught_transitive_raise(true)
p caught_transitive_raise(false)

p div_may_raise(10, 0)
p div_may_raise(10, 2)
p div_safe_literal(11)
p mod_float_may_raise(5.0, 0.0)
p mod_float_may_raise(5.5, 2.0)

p type_error_unbox("nope")
p type_error_unbox(4)

p nomethod_nil_recv(false)
p nomethod_nil_recv(true)
p nomethod_boxed_recv("no_such")
p nomethod_boxed_recv(b)

mb = MutableBox.new(1)
mb.freeze
p frozen_ivar_write(mb)

p raising_yield_wrap(1) { |x| raise "from_block" if x == 1; "ok" }
p raising_yield_wrap(2) { |x| raise "from_block" if x == 1; "ok" }
p nomethod_undef_method(UndefHost.new)
