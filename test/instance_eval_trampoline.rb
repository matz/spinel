# Inline-at-call-site arity-0 instance-eval trampoline.
#
# Full Ruby instance-eval is dynamic — `self` is rebound at runtime,
# so AOT compilation needs static type information to resolve method
# dispatch inside the block. Spinel's compromise: detect the exact
# DSL trampoline shape `def m(&b); instance_eval(&b); end` at compile
# time, and inline the block body at the call site with `self`
# rebound to the receiver. Receiverless calls inside the spliced
# body dispatch to the receiver's class via static type inference
# (the new `@instance_eval_self_var` / `@instance_eval_self_type`).
#
# Anything other than the arity-0 single-statement shape falls
# through to the previous silent no-op — by design.

# 1. Basic trampoline: `recv.configure { add 10 }` rebinds self to
#    the Builder instance, so `add(10)` resolves as Builder#add.
class Builder
  def initialize
    @sum = 0
  end

  def add(n)
    @sum = @sum + n
  end

  def total
    @sum
  end

  def configure(&block)
    instance_eval(&block)
  end
end

b = Builder.new
b.configure do
  add(10)
  add(20)
  add(12)
end
puts b.total              #=> 42

# 2. Multiple statements inside the block — each receiverless call
#    dispatches against the rebound class, including reads.
class Counter
  def initialize
    @n = 0
  end

  def bump
    @n = @n + 1
  end

  def get
    @n
  end

  def with(&block)
    instance_eval(&block)
  end
end

c = Counter.new
c.with do
  bump
  bump
  bump
end
puts c.get                #=> 3

puts "done"
