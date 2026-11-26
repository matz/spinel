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

# a named keyword whose slot may hold nil is boxed as the slot holds it,
# into a builtin's arm and into a parent that takes a rest
class N
  def is_a?(c, k: nil) = super
  def respond_to?(m, k: nil, f: nil)
    f = 2.5 if m == :zz
    super
  end
  def self.include?(m, k: nil)
    k = 3 if m == Kernel
    -> { k = nil }.call if m == Comparable
    super
  end
end
n = N.new
try { n.is_a?(N) }
try { n.is_a?(N, k: 1) }
try { n.respond_to?(:is_a?) }
try { n.respond_to?(:zz, k: 5) }
try { N.include?(Kernel) }
try { N.include?(Comparable, k: 4) }
class PR
  def m(*a) = a
end
class CR < PR
  def m(x, k: nil, j: 1.5)
    j = nil if x == 0
    super
  end
end
p CR.new.m(1), CR.new.m(0, k: 2), CR.new.m(3, k: 4, j: 2.0)
# a parent taking `**`: the keywords it names none of go into its Hash,
# each boxed as its slot holds it
class PK
  def m(*a, **kw) = [a, kw]
  def f(**kw) = kw
end
class CK < PK
  def m(x, k: nil, j: 1.5)
    j = nil if x == 0
    super
  end
  def f(k: nil, **o)
    k = 3 if k == 0
    super
  end
end
class DK < CK; end
p CK.new.m(1), CK.new.m(0, k: 2), DK.new.m(3, k: 4, j: 2.0)
p [CK.new.f, CK.new.f(k: 0), DK.new.f(k: 7, z: 1)]
