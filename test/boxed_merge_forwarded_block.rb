# `h.merge(o, &block)` with h held boxed and the block a method's own,
# passed on -- activesupport's Hash#deep_merge hands its block to
# deep_merge! this way. The boxed merge read only a block written at the
# call, so a forwarded one, or none, raised NoMethodError.
def mrg(h, o, &b) = h.merge(o, &b)
x = [{ a: 1 }, [1]][ARGV.size]
p mrg(x, { a: 2 }) { |_k, a, b| a + b }
p mrg(x, { c: 2 })
p mrg(x, { a: 5, d: 1 }) { |k, a, b| "#{k}:#{a}/#{b}" }
