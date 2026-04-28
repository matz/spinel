# Self-call `m { ... }` &block forwarding (Site A).
#
# When a method on class C calls another method on the same class
# without an explicit receiver (`m { ... }` instead of `self.m { ... }`),
# dispatch goes through `compile_no_recv_call_expr`'s
# `@current_class_idx >= 0` branch — call this Site A. Pre-fix this
# branch always emitted `sp_C_m(self, ca)` with no block tail, so a
# literal block at the call site was silently dropped and the binary
# segfaulted in `sp_proc_call(NULL, ...)`.
#
# Site A reuses the `has_literal_block` helper and the
# `omit_trailing` 4th arg of `compile_typed_call_args` introduced
# in the previous PR (Site B / typed-receiver forwarding); the only
# new code here is the same `bp`/`tail` template adapted to the
# self-call form.

# 1. Bare self-call dispatch — one method invokes another method
#    of the same class with a literal block.
class Caller
  def kick
    inner { puts "1-self-call" }
  end

  def inner(&block)
    block.call
  end
end

Caller.new.kick

# 2. Self-call passes a value through to the block via block.call(arg).
class Wrapper
  def go
    forward(7) { |i| puts "2-arg=#{i}" }
  end

  def forward(n, &block)
    block.call(n)
  end
end

Wrapper.new.go

# 3. Self-call inside a loop — each iteration forwards a fresh block.
class Bank
  def run
    deposit_each(3) { |i| puts "3-dollar-#{i}" }
  end

  def deposit_each(n, &block)
    i = 0
    while i < n
      block.call(i)
      i = i + 1
    end
  end
end

Bank.new.run

puts "done"
