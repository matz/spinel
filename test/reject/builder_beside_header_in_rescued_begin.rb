# A builder and a header of the same name, where the header is in a begin
# body that has a rescue: a statement ahead of the header can raise, the
# rescue catches it, and the header does not run. CRuby's class then has
# no `k`. spinel cannot tell, so the program is refused.
module Outer
  Foo = Class.new { def b = 1 }
  begin
    raise "x" if ARGV.empty?
    class Foo; def k = 2; end
  rescue
  end
end
module Other
  class Foo; def k = 8; end
end
p((Outer::Foo.new.k rescue :nok))
