# A class that includes a module inside a block, where the module defines
# its method again under a condition there: a call runs the version the
# module holds when the call runs, told by the module's cells, and a super
# in a later version reaches past the module.
module V; def a = 1; end
class P; def a = :p; end
r = []
2.times do |i|
  class K < P; include V; end
  r << K.new.a
  module V; def a = [super, 2]; end if i == 0
  r << K.new.a
end
p r

# the later version calls no super
module W; def b = 1; end
s = []
2.times do |i|
  class L; include W; end
  s << L.new.b
  module W; def b = 2; end if i == 0
end
p s
