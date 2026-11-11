# An instance_eval body on a boxed receiver, compiled after a value-position
# attribute setter made a node of its own: the body's self is the boxed value,
# so a bare `name` is a NameError, as in CRuby, not the one class's reader.
class Pre
  def x=(v)
    @x = v
  end
end
class Named
  attr_reader :name
  def initialize = @name = "n"
end
def t = (p yield)
r = (Pre.new.x = [7].first)
v = [1, "s"][0]
t { v.instance_eval { name } rescue $!.class }
t { Named.new.name }
p r
