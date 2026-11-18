# Two conditional definitions of String#first make a dispatcher whose else arm
# raises; a receiverless `first` elsewhere (Enumerable's inject, through a
# class that includes it) gives the dispatcher a variable-call twin whose
# NameError names `self.class` at run time, since String has a subclass here.
# A String method's self is a handle once a method changes it in place
# (shout!), and the twin boxes that self for the class it names.
unless $__loaded_a
class String
  def first(limit = nil); raise(NotImplementedError, "pruned a") end
  def shout!; self << "!"; self end
end
end
unless $__loaded_b
class String
  def first(limit = nil); raise(NotImplementedError, "pruned b") end
end
end
class Lister
  def first; 1; end
  def go; first; end
end
p Lister.new.go
s = +"hello"
s.shout!
p s
begin
  s.first(2)
rescue NotImplementedError => e
  p e.message
end
class Bag
  include Enumerable
  def each; yield 1; yield 2; yield 3; end
end
p Bag.new.inject { |a, x| a + x }
class SafeBuf < String
  def html_safe?; true; end
end
sb = SafeBuf.new("safe")
sb.shout!
p sb, sb.html_safe?
begin
  sb.first(1)
rescue NotImplementedError => e
  p e.message
end
