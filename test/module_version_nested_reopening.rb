# A reopening nested in a module's body runs where that body does: its
# include or prepend is as certain as a top-level one. A call made before
# it reaches the class's method, a call after it the module's, and a
# later version of the module's method is a version in the class.
module NS
  module P; def get_x = 1; end
  class K; def native = 1; end
end
k = NS::K.new
p((k.get_x rescue :none))
module NS
  class K
    include P
  end
end
p k.get_x, k.respond_to?(:get_x)

module NS
  module Q; def get_y = [:q1, super]; end
  class J; def get_y = :j; end
end
j = NS::J.new
r = [j.get_y]
module NS
  class J; prepend Q; end
end
r << j.get_y
module NS::Q; def get_y = [:q2, super]; end
r << j.get_y
p r

class LB; def get_z = :lb; end
class Outer
  class L < LB; def native = 1; end
end
module R; def get_z = [:r, super]; end
l = Outer::L.new
s = [l.get_z]
class Outer
  class L; include R; end
end
s << l.get_z
p s
