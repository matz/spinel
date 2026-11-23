# a table-loop define_method whose parameter has a default, beside an included hook that defines a method (a slot the hook splice shared with the loop answered [:a, nil])
class T
  [:a, :b].each { |nm| define_method(nm) { |q = 0| [nm, q] } }
end
p T.new.a
module H
  def self.included(b); b.define_method(:hy) { 1 }; end
end
class U; include H; end
p U.new.hy
