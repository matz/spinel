# Multi-argument `block.call(a, b, ...)` on the proc-object path.
#
# Pre-fix, `compile_proc_literal` hardcoded the lambda's signature
# as `static mrb_int <fn>(void *_cap, mrb_int lv_<bp>)` — a single
# `mrb_int` slot — and the call site always lowered to
# `sp_proc_call(lv_<rname>, <arg0>)`, so any second/third positional
# arg was silently dropped.
#
# Fix collects every block param at proc-literal time and emits
# `(void *_cap, mrb_int lv_<bp1>, mrb_int lv_<bp2>, ...)`, then
# dispatches the call site to `sp_proc_call_N` (added to the
# runtime: cast the stored function pointer to the matching N-arg
# signature and invoke).
#
# Out of scope: arity-mismatched calls (`proc { |a| } .call(1, 2)`)
# stay UB on the C side. Spinel's static dispatch enforces match in
# the test cases below.

# 1. Basic: forwarded `&block` invoked with 2 args.
class App
  def run(&block)
    block.call(10, 20)
  end
end

App.new.run { |a, b| puts a + b }

# 2. 3-arg block.call.
class Wider
  def go(&block)
    block.call(1, 2, 3)
  end
end

Wider.new.go { |x, y, z| puts x + y + z }

# 3. Same proc invoked twice with different concrete args.
class Twice
  def both(&block)
    block.call(100, 1)
    block.call(200, 2)
  end
end

Twice.new.both { |a, b| puts a * b }

# 4. 0-param block called with one arg. Extra arg is silently dropped
# (matches CRuby semantics). Exercises the `_unused` fallback path
# that the multi-param refactor must leave intact.
class Drop
  def go(&block)
    block.call(99)
  end
end

Drop.new.go { puts "called" }
