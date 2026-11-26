# A bare super into a builtin arm passes a keyword parameter's value as it is
# when the super runs, a closure's write to it included: the `**` a lambda
# emptied passes nothing more.
def try
  p yield
rescue ArgumentError => e
  p e.message
end
module M; end
class K
  include M
  def is_a?(c, **kw)
    -> { kw = {} }.call
    super
  end
  def respond_to?(m, **kw)
    -> { kw = {} }.call
    super
  end
  def self.include?(m, **kw)
    -> { kw = {} }.call
    super
  end
  private def hidden = 1
end
k = K.new
try { k.is_a?(K, a: 1) }
try { k.respond_to?(:hidden, a: 1) }
try { k.respond_to?(:is_a?, a: 1) }
try { K.include?(M, a: 1) }
