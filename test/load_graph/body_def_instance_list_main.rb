# A list of a class's own instance methods (`K.instance_methods(false)`
# and its public_, protected_ and private_ forms) on a constant reads the
# version cells of a method a class method's body defines, as a call of
# it there finds it: the name is listed once the body has run.
class K
  def self.setup
    def m = :m
    def n = :n
  end
  def o = :o
  protected def pr = :pr
  private def pv = :pv
end
def show(tag)
  p [tag, K.instance_methods(false).sort, K.public_instance_methods(false).sort,
     K.protected_instance_methods(false).sort, K.private_instance_methods(false).sort,
     K.public_instance_methods(false).include?(:m), (K.new.m rescue $!.class), K.new.respond_to?(:m)]
end
show(:before)
K.setup
show(:after)

# At a point no instance of the class exists yet, a list or a
# method_defined? still asks the class: a body's method is not there yet.
class J
  def self.setup = (def j = :j)
end
class S < J
  def s = :s
end
lists = proc { [J.instance_methods(false), S.instance_methods(false)] }
p [J.method_defined?(:j), J.public_method_defined?(:j), J.instance_methods(false), lists.call]
J.setup
p [J.method_defined?(:j), J.public_method_defined?(:j), J.instance_methods(false), lists.call, S.new.j]

# A placed def and a body's def of one name: the list shows the name
# throughout, and a call answers the version live there.
class R
  def m = :old
  def self.setup = (def m = :new)
end
def rshow(tag) = p([tag, R.instance_methods(false), R.new.m])
rshow(:a)
R.setup
rshow(:b)
