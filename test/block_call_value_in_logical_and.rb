# A call of a block parameter, which a caller's literal block takes the place
# of, is the right operand of `&&` in a block of a method that is also
# reached by respond_to?: the `&&` answers a boxed value there (false or the
# block's), so the spliced block's own value is boxed too, where it was left
# a raw Integer that the boxed slot of the block value could not take.
class Big < Array
  def sel(&b) = select { |v| v > 1 && b.call(v) }
end
p Big[1, 2].sel { |v| true }
p Big[1, 2, 3].sel { |v| v > 2 }
p Big[1, 2].respond_to?(:sel)

class Pick < Array
  def select(&b) = super { |v| v > 1 && b.call(v) }
end
p Pick[1, 2, 3].select { |v| v < 3 }
p Pick[1, 2].respond_to?(:select)

class Pairs < Hash
  def select(&b) = super { |k, v| v > 1 && b.call(k, v) }
end
h = Pairs.new
h[:a] = 1
h[:b] = 2
h[:c] = 3
p h.select { |k, v| k != :c }
p h.respond_to?(:select)
