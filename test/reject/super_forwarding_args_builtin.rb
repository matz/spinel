# spinel: reject-unsupported: `...` forwarded by `super(...)` into the builtin `is_a?` is not supported
# A `super(...)` from a method whose `...` its calls pass in more than one
# form reaches the builtin's arm with a list no arm reads: refused rather
# than answered by a NoMethodError CRuby does not raise.
class K
  def is_a?(...) = super(...)
end
k = K.new
p k.is_a?(K)
begin
  p k.is_a?(K, 1)
rescue ArgumentError => e
  p e.message
end
