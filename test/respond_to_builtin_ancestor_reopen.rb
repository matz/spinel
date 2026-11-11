# respond_to? on a builtin value for a method the program adds to a
# builtin module or class among its ancestors

module Comparable
  def cmp_pub = 1
  private def cmp_priv = 2
  protected def cmp_prot = 3
end
module Enumerable
  def enum_pub = 4
end
class Numeric
  def num_pub = 5
end
class Pt
  include Comparable
  def <=>(o) = 0
end
class Plain; end

# typed receivers
p "s".respond_to?(:cmp_pub), 3.respond_to?(:cmp_pub), 1.5.respond_to?(:cmp_pub)
p :y.respond_to?(:cmp_pub), Time.at(0).respond_to?(:cmp_pub), 3r.respond_to?(:cmp_pub)
p (10**30).respond_to?(:cmp_pub), Complex(1, 2).respond_to?(:cmp_pub)
p "s".respond_to?(:cmp_priv), "s".respond_to?(:cmp_priv, true)
p "s".respond_to?(:cmp_prot), "s".respond_to?(:cmp_prot, true)
p 3.respond_to?(:num_pub), 1.5.respond_to?(:num_pub), 3r.respond_to?(:num_pub), "s".respond_to?(:num_pub)
p (1..3).respond_to?(:enum_pub), [1].respond_to?(:enum_pub), {a: 1}.respond_to?(:enum_pub)
p (1..3).respond_to?(:cmp_pub), [1].respond_to?(:cmp_pub), nil.respond_to?(:cmp_pub), true.respond_to?(:cmp_pub)
p "s".respond_to?(:nope), 3.respond_to?(:enum_pub)
p "s".cmp_pub, 3.num_pub, (1..2).enum_pub

# boxed receivers
vals = [1, 2.5, "s", :y, nil, true, [1], {a: 1}, (1..2), Pt.new, Plain.new, 10**30, Time.at(0), 3r]
vals.each do |v|
  p [v.class, v.respond_to?(:cmp_pub), v.respond_to?(:enum_pub), v.respond_to?(:num_pub),
     v.respond_to?(:cmp_priv), v.respond_to?(:cmp_priv, true)]
end
def any_cmp(x) = x.respond_to?(:cmp_pub)
p any_cmp("s"), any_cmp(2), any_cmp([1])

# a String that may be nil: nil answers its own surface
def maybe_s(f) = f ? "s" : nil
p maybe_s(false).respond_to?(:cmp_pub), maybe_s(true).respond_to?(:cmp_pub)
p ["q"][5].respond_to?(:cmp_pub)

# a second argument known only at run time
p "s".respond_to?(:cmp_priv, ARGV.size > 5)

# a nearer ancestor hides the name: an undef, or a private def
module Comparable
  def hid = 1
  def nhid = 2
end
class String
  undef_method :hid
end
class Numeric
  private def nhid = 3
end
p "s".respond_to?(:hid), 3.respond_to?(:hid), 3.respond_to?(:nhid), "s".respond_to?(:nhid)
["s", 3, 1.5, :y].each { |x| p [x.respond_to?(:hid), x.respond_to?(:nhid)] }
