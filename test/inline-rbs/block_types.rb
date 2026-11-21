# Signatures with a block type (`{ (T) -> U }`) over methods that yield or
# call a `&block`. The block type is not seeded -- the block's parameters and
# result stay inferred, as --rbs leaves them -- but the positional and return
# types still apply, and the program compiled with `--no-inline-rbs --rbs
# sig/block_types` produces the same C byte for byte. Each annotation pins a
# slot to `untyped`, which inference would not choose on its own, so the C
# shows whether it applied.
class Walker
  #: (untyped) { (Integer) -> void } -> untyped
  def each_upto(n)
    i = 0
    while i < n
      yield i
      i += 1
    end
    n
  end

  #: (untyped) { (String) -> bool } -> untyped
  def pick(xs, &block) = xs.select { |x| block.call(x) }

  #: (untyped, Integer) { (String, Integer) -> Integer } -> untyped
  def fold(xs, base)
    s = base
    xs.each_with_index { |x, i| s += yield(x, i) }
    s
  end
end

w = Walker.new
p w.each_upto(3) { |i| print i, " " }
p w.pick(["a", "bb", "ccc"]) { |x| x.length > 1 }
p w.fold(["a", "bb"], 10) { |x, i| x.length + i }
