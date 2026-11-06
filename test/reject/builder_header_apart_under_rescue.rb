# A builder and a header of the same name with a statement between them,
# inside a begin with a rescue: the statement can raise, the rescue catches
# it, and the header does not run. spinel cannot tell, so it refuses.
begin
  module Outer
    Foo = Class.new { def b = 1 }
    raise "x" if ARGV.empty?
    class Foo; def k = 2; end
  end
rescue
end
module Other
  class Foo; def k = 8; end
end
p((Outer::Foo.new.k rescue :nok))
