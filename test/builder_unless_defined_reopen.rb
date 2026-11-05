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
