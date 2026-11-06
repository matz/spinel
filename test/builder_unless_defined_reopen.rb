# `Bar = Class.new { } unless defined?(Bar)`, then `class Bar` in the same
# body: the write runs (nothing else defines Bar), so the header reopens
# the built class, which has both methods. Also with a namesake elsewhere.
module Outer
  Bar = Class.new { def b = 1 } unless defined?(Bar)
  class Bar; def k = 2; end
end
module Other
  class Bar; def k = 8; end
end
o = Outer::Bar.new
p o.k, o.b
p Other::Bar.new.k
p((Other::Bar.new.b rescue :nob))
# a builder and its header in one list of statements run together, under
# an if or a begin
if __FILE__ == $0
  Baz = Class.new { def b = 3 }
  class Baz; def k = 4; end
  p Baz.new.k, Baz.new.b
end
begin
  Qux = Class.new { def b = 5 }
  class Qux; def k = 6; end
end
p Qux.new.k, Qux.new.b
# a write of the same name in a body the write cannot see does not stop it
module Inner
  Bar = Class.new { def b = 7 } unless defined?(Bar)
  class Bar; def k = 8; end
end
module Cfg
  Bar = 9
end
module Mod
  Bar = Class.new { def j = 10 }
end
p Inner::Bar.new.k, Inner::Bar.new.b, Cfg::Bar, Mod::Bar.new.j
