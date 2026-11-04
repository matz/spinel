# m::Foo with m a block parameter names the Foo of each module in turn:
# CRuby prints false, then true. Two modules nest a Foo, and Spinel
# resolves the path by its leaf name, so the program is refused instead
# of printing false twice.
module Ext; class Foo; end; end
module Oth; class Foo; end; end
x = Oth::Foo.new
[Ext, Oth].each { |m| p x.is_a?(m::Foo) }
