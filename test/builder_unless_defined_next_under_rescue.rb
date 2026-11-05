# A header right after a builder guarded by `unless defined?`, in a begin
# with a rescue: no statement between them can raise, so it compiles.
module Outer
  begin
    Foo = Class.new { def b = 1 } unless defined?(Foo)
    class Foo; def k = 2; end
  rescue
  end
end
p Outer::Foo.new.k
