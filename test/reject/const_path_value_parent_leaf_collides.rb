# m::Foo with m a local names the Foo that m's module holds: CRuby prints
# "ext". Spinel resolves the path by its leaf name, and a top-level Foo
# shares it, so the program is refused instead of printing "top".
class Foo; def who = "top"; end
module Ext; class Foo; def who = "ext"; end; end
m = Ext
puts m::Foo.new.who
