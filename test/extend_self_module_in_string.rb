# A module that says `extend self` and is mixed into String: its methods run
# on a String, and the module's own copies of them, which nothing calls, are
# not compiled (in CRuby they would raise only if called).
module Paint
  extend self
  def red = wrap(self)
  def wrap(s) = +"<" << s
  def to_eol = sub(/a/, "b")
end
String.send(:include, Paint)
p "a".red
p "abc".to_eol

# a class method of the same name as an instance method a String reaches
module Tag
  def self.em = mark(self)
  def self.mark(s) = +"*" << s
  def em = mark(self)
  def mark(s) = +"_" << s
end
String.include(Tag)
p "x".em

# the module's own copy is still the one a call on the module runs
module Both
  extend self
  def twice(s) = s + s
  def shout = twice("ho")
end
class String; include Both; end
p Both.shout
p Both.twice("a")
p "z".shout

# and a class that extends the module runs its methods as class methods
module Helper
  def label = prefix + name
  def prefix = "class "
end
class Widget
  extend Helper
end
p Widget.label
