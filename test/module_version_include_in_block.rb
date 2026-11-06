# A reopening's include inside a block, under a condition: a call before
# it has run reaches the class's method, a call after it the version the
# module holds then, and a super in a later version reaches past the
# module.
module V; def a = 1; end
class P; def a = :p; end
class K < P; end
r = []
2.times do |i|
  r << K.new.a
  class K; include V; end if i == 0
  r << K.new.a
end
p r
module V; def a = [super, 2]; end
p K.new.a

# a plain module: no later version
module W; def b = :w; end
class Q; def b = :q; end
class L < Q; end
s = []
[1, 2].each do |i|
  s << L.new.b
  class L; include W; end if i == 1
end
p s
