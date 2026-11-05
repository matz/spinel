# A header right before its builder, in a begin with a rescue. If the
# builder raises, the header has run and the write has not, so the header
# does not reopen the built class. spinel cannot tell, so it refuses.
module Outer
  begin
    class Foo; def k = 2; end
    Foo = Class.new { def b = 1 }
  rescue
  end
end
p((Outer::Foo.new.k rescue :nok))
