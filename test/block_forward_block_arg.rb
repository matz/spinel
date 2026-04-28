# `&proc_var` argument forwarding (`def m(&b); g(&b); end`).
#
# Pre-fix Spinel never parsed `&expr` in call argument position
# (`g(&block)`) — Prism's `PM_BLOCK_ARGUMENT_NODE` had no case in
# `flatten()`, so the codegen saw the call as taking no block at all
# and the captured `&block` was silently dropped at every forwarding
# call site.
#
# This PR adds:
#   - parser case for PM_BLOCK_ARGUMENT_NODE → "BlockArgumentNode"
#   - `find_block_arg(nid)` helper that returns the inner expression
#   - `strip_block_arg` filter so &block-args don't leak into the
#     positional-args comma list at compile_call_args / -with_defaults
#     / compile_typed_call_args
#   - extends Sites A, B, and the receiverless has_block_param path
#     to fall through to find_block_arg when has_literal_block returns 0,
#     emitting the proc expression directly (no compile_proc_literal call,
#     since the proc is already a captured `sp_Proc *`).

# 1. Site A (self-call) &proc_var forwarding.
class Forwarder
  def outer(&block)
    inner(&block)
  end

  def inner(&block)
    block.call
  end
end

Forwarder.new.outer { puts "1-self-amp" }

# 2. Site B (typed-receiver) &proc_var forwarding — outer captures
#    &block, then forwards it to a different receiver's method.
class Sink
  def receive(&block)
    block.call
  end
end

class Source
  def relay(sink, &block)
    sink.receive(&block)
  end
end

Source.new.relay(Sink.new) { puts "2-recv-amp" }

# 3. Top-level (receiverless) `&proc_var` forwarding.
def outer_top(&block)
  inner_top(&block)
end

def inner_top(&block)
  block.call
end

outer_top { puts "3-top-amp" }

puts "done"
