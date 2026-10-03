# A prepended module that defines a method the class defines too renames the
# class's own body (so the module's copy can super into it), and reflection
# skipped the renamed body: the class's own method vanished from
# instance_methods(false). It is listed under its own name, with its
# visibility, through one prepend or two.
module A
  def greet = "A" + super
end
module B
  def greet = "B" + super
  def tick = 0
end

class K
  prepend A
  prepend B
  def greet = "K"
  def tick = 9
  def plain = 2
  private def hush = 3
end

p K.new.greet, K.new.tick
p K.instance_methods(false).sort
p K.public_instance_methods(false).sort
p K.private_instance_methods(false).sort
p K.method_defined?(:greet)

class L
  prepend A
  private def greet = "L"
  def other = 1
end

p L.instance_methods(false).sort
p L.private_instance_methods(false).sort
