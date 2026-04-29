# Typed-receiver `recv.m { ... }` &block forwarding (Site B).
#
# Pre-fix `compile_object_method_expr` dropped the literal block: the
# call site emitted `sp_Cls_m(rc, 0)` and the binary segfaulted inside
# `sp_proc_call(NULL, ...)`. Fix: precompute `has_proc` from the
# callee's param types, pass it to `compile_typed_call_args` as the
# new `omit_trailing` 4th arg so the &block slot isn't padded with
# "0", then build a `bp`/`tail` that appends the proc literal after
# the regular args.

# 1. Basic: `recv.m { ... }` with arity-0 block.call.
class App
  def run(&block)
    block.call
  end
end

App.new.run { puts "1-basic" }

# 2. block.call with one int arg, return value used.
class Doubler
  def apply(x, &block)
    block.call(x)
  end
end

puts Doubler.new.apply(21) { |n| n * 2 }

# 3. Mixed: regular arg + &block, multiple stmts in method body.
class Logger
  def log(label, &block)
    puts label
    block.call
  end
end

Logger.new.log("3-mixed") { puts "  body" }

# 4. Block invoked multiple times from inside the method.
class Repeater
  def thrice(&block)
    block.call
    block.call
    block.call
  end
end

Repeater.new.thrice { puts "4-thrice" }

# 5. Block uses interpolation of its own argument. (Closure capture
#    over outer locals is a separate subsystem — out of scope here.)
class Wrapper
  def go(n, &block)
    block.call(n)
  end
end

Wrapper.new.go(7) { |i| puts "5-arg=#{i}" }

# 6. Block returns a value used by the method.
class Picker
  def pick(&block)
    block.call(10) + block.call(20)
  end
end

puts Picker.new.pick { |n| n * 5 }

# 7. Method declares &block, call site provides none. The &block
#    slot must be filled with NULL — pre-fix `omit_trailing` dropped
#    the slot from the C call entirely, leaving an arity mismatch
#    between sp_Optional_maybe(sp_Optional *, sp_Proc *) and
#    sp_Optional_maybe(_t1).
class Optional
  def maybe(&block)
    puts "7-no-block"
  end
end

Optional.new.maybe

puts "done"
