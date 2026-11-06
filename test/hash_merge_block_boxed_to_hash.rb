# Hash#merge with a block, with an argument read out of an Array: a
# program object merges its #to_hash, and any other value than a Hash
# raises TypeError.
class E; def to_hash = {7 => 8}; end
class Bad; def to_hash = 5; end
class None; end
class HS < Hash; end
h = {7 => 1, "a" => :b}
[E.new, {7 => 3}, Bad.new, None.new, 5].each do |q|
  begin
    p h.merge(q) { |k, o, n| [o, n] }
  rescue TypeError => e
    p [e.class, e.message]
  end
end
hs = HS.new; hs[7] = 70
[E.new, hs].each { |q| p h.merge(q) { |k, o, n| [o, n] } }
g = {1 => 2}
hs2 = HS.new; hs2[1] = 10
[E.new, {1 => 5}, hs2].each { |q| p g.merge(q) { |k, o, n| o + n } }
[E.new, {7 => 4}].each { |q| p h.merge(q, {9 => 9}) { |k, o, n| [o, n] } }
[hs2, nil].each { |q| p((g.merge(q) { |k, o, n| o + n } rescue $!.class)) }
