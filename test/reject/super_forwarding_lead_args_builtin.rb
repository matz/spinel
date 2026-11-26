# spinel: reject-unsupported: `...` forwarded by `super(...)` into the builtin `respond_to?` is not supported
# A `super(m, ...)` hands on a leading parameter and the method's `...`,
# whose list no arm of the builtin reads, even where every call passes one
# count: refused rather than answered by a NoMethodError CRuby does not raise.
class K
  def respond_to?(m, ...) = super(m, ...)
end
p K.new.respond_to?(:is_a?)
