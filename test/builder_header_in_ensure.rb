# A header in a begin's ensure runs whenever the begin runs, so it reopens
# the class the builder beside it made.
module Outer
  Foo = Class.new { def b = 1 }
  begin
    x = 1
  rescue
    x = 2
  ensure
    class Foo; def k = 2; end
  end
end
module Other
  class Foo; def k = 8; end
end
p Outer::Foo.new.b
p((Outer::Foo.new.k rescue :nok))
p Other::Foo.new.k
