# Hash#merge with a block on a Hash of mixed keys and values reads an
# object argument through its #to_hash, as CRuby does.
class E
  def to_hash = {7 => 8}
end
class F
  def to_hash = {7 => 9}
end

p({1 => 2}.merge(E.new, F.new) { |k, o, n| o + n })
h = {1 => 2, "a" => :b}
p h.merge(E.new) { |k, o, n| [o, n] }
p h.merge(E.new, F.new) { |k, o, n| [o, n] }
p h
p({7 => 1, "a" => :b}.merge(E.new) { |k, o, n| [k, o, n] })
p({7 => 1, "a" => 2}.merge(E.new) { |k, o, n| o + n })
p({7 => "x", 8 => 2}.merge(E.new) { |k, o, n| [o, n] })
p({7 => 1.5}.merge(E.new) { |k, o, n| [o, n] })
class Base; end
class Kid < Base
  def to_hash = {7 => 5}
end
def via(x) = {7 => 1, "a" => :b}.merge(x) { |k, o, n| [o, n] }
p via(Kid.new)
# a slot that may hold nil raises, as CRuby's merge(nil) does
y = F.new
y = nil if ARGV.size == 0
p(({7 => 1, "a" => :b}.merge(y) { |k, o, n| [o, n] } rescue $!.message))
