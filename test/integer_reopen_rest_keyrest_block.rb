# A reopen of Integer#times with *rest, **rest and a block parameter takes
# every call: positional, keyword and a block, as CRuby runs it.
class Integer
  def times(*a, **kw, &b) = [a, kw, b&.call]
end
p 3.times(1, 2, a: 1) { :blk }
p 3.times { :b }
p 3.times({a: 1})
