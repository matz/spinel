# Struct.new(*keys) / Data.define(*keys) over a list the program only reads
# (size, include?, first, sort, a splat, map(&:to_sym)) or freezes keeps its
# names at compile time. A list the program changes in place is refused
# (test/reject/struct_new_member_list_changed.rb).
k = %i[b a]
p k.size, k.include?(:a), k.first
S = Struct.new(*k.sort)
p S.members
T = Struct.new(*k)
p T.members

KEYS = %i[x y].freeze
KEYS.each { |s| p s }
P = Struct.new(*KEYS)
p P.new(1, 2)

DK = %i[u v]
D = Data.define(*DK)
p D.new(u: 1, v: 2), DK.length

w = %w[p q]
W = Struct.new(*w.map(&:to_sym))
p W.members, w.size

# a local of the same name in another method changes its own list
def other
  k = [1]
  k << 2
  k
end
p other
