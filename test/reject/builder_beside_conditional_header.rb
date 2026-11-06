# A builder and a header of the same name in one body, where the header is
# under its own `if`: when the header does not run, CRuby's class has no
# `k`. spinel cannot tell, so the program is refused.
module Outer
  Foo = Class.new { def b = 1 }
  if ARGV.size > 0
    class Foo; def k = 2; end
  end
end
module Other
  class Foo; def k = 8; end
end
p((Outer::Foo.new.k rescue :nok))
