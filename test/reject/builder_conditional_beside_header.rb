# `Bar = Class.new { }` under an `if` that may be false, then `class Bar`
# in the same body: CRuby's header reopens the built class only when the
# write ran, and makes a fresh class when it did not. Spinel cannot tell
# which, and merged the two (o.b answered 1 where CRuby raises
# NoMethodError), so the program is refused.
module Outer
  Bar = Class.new { def b = 1 } if ARGV.size > 5
  class Bar; def k = 2; end
end
module Other
  class Bar; def k = 8; end
end
o = Outer::Bar.new
p o.k
p((o.b rescue :nob))
