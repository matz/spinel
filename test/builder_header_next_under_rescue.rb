# A header right next to its builder runs with it, even under a rescue:
# no statement between them can raise.
module Outer
  begin
    Foo = Class.new { def b = 1 }
    class Foo; def k = 2; end
  rescue
  end
end
module Other
  class Foo; def k = 8; end
end
p Outer::Foo.new.b
p Outer::Foo.new.k
p Other::Foo.new.k
