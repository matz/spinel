# Anonymous `&` block forwarding (Ruby 3.1+).
#
# `def outer(&); inner(&); end` declares an unnamed block parameter
# and forwards it via the matching anonymous `&` in the call. Pre-fix,
# the parameter `BlockParameterNode { name: nil }` was registered with
# an empty name, leaving the method without a `_block`/`lv_` slot, and
# the forwarding `BlockArgumentNode { expression: -1 }` got dropped at
# the call site (it slipped through `find_block_arg`'s -1 return).
#
# Fix synthesizes the internal name `__anon_block` for unnamed block
# params so they flow through `find_block_param_name` and
# `@current_method_block_param` like any other `&block`, and extends
# `block_forward_expr` to forward the current method's anon-block
# slot when it sees a BlockArgumentNode with no expression.

# 1. Top-level outer/inner with anonymous & forwarding.
def outer(&)
  inner(&)
end

def inner(&block)
  block.call
end

outer { puts "1-top-level" }

# 2. Anonymous & with a body that does work before forwarding.
def with_prefix(label, &)
  puts label
  inner(&)
end

with_prefix("2-prefix") { puts "  body" }

# 3. Anonymous & on a class instance method, forwarding to another
#    class instance method via typed-receiver dispatch.
class Outer
  def kick(&)
    sink(&)
  end

  def sink(&block)
    block.call
  end
end

Outer.new.kick { puts "3-class-method" }
