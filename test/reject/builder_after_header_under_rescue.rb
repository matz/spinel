# A header right before its builder, in a begin with a rescue. The header
# runs first and makes its own class, which the write then replaces, so
# spinel refuses it (see builder_after_header.rb), rescue or not.
module Outer
  begin
    class Foo; def k = 2; end
    Foo = Class.new { def b = 1 }
  rescue
  end
end
p((Outer::Foo.new.k rescue :nok))
