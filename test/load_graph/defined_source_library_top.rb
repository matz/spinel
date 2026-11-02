# defined?(ERB::Escape) outside any class or module body, after the require
# of a library spinel provides from Ruby source (packages/erb), asks the
# library's own class, which its file defines ahead of the guard: the
# constant ERB::Escape, not a top-level Escape (the program has none). The
# guard picks the same branch as CRuby whether it picks a pair of top-level
# defs, a ternary or an && operand. A name the source does not define
# (ERB::NoSuch) is absent, though the program writes a NoSuch of its own at
# the top level. A remove_const of some other constant, even one that is
# never called, leaves the library's constant alone.
require "erb"

def never_called = Object.send(:remove_const, :Zork)
def never_called_either = Object.remove_const(:Zork)

if defined?(ERB::Escape)
  def top_h = "then"
else
  def top_h = "else"
end
p top_h

x = defined?(ERB::Escape) ? "then" : "else"
p x

class K
  def self.ok = "then"
end
p(defined?(ERB::Escape) && K.ok)
p((defined?(ERB::Util) ? 1 : 2))
p((defined?(ERB::NoSuch) ? 1 : 2))
p(defined?(ERB::NoSuch) && K.ok)

if defined?(ERB::Escape) && ERB::Util.respond_to?(:h)
  def both = "both"
else
  def both = "one"
end
p both

NoSuch = Class.new
p((defined?(ERB::Escape) ? 1 : 2))
p((defined?(ERB::NoSuch) ? 1 : 2))
p((defined?(ERB::Escape::Foo) ? 1 : 2))
