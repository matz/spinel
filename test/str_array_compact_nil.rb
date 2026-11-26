# compact! and compact on an Array of Strings that holds nil (a `map`
# block's `v if c` answers one): the nils are dropped, compact! in place
# and answering self, or nil when there was none
s = %w[a b c].map { |v| v if v != "b" }
b = s.dup
p b.compact!.equal?(b), b, b.size
p s.compact, s
p s.compact!, s.compact!
t = %w[x y].map { |v| v }
p t.compact!, t
f = %w[q].map { |v| v if v == "z" }.freeze
begin
  f.compact!
rescue => e
  p e.class
end
