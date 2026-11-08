# `alias send_action send` names Kernel's send, which no class of the
# program defines: a call of the alias raised NoMethodError, and so did a
# `super` into it from a subclass's or an included module's method of the
# name. actionpack's AbstractController::Base has the alias, and
# BasicImplicitRender's send_action calls super into it.
class Base
  def process(m, *a) = send_action(m, *a)
  alias send_action send
  alias_method :call_public, :public_send
  alias dispatch_to __send__

  def hi(x) = "hi #{x}"
  def kw(a:, b: 2) = a + b
  def blk = yield(3)

  private

  def secret = :secret
end

module Rendering
  def send_action(m, *a)
    ret = super
    "#{ret}!"
  end
end

class Controller < Base
  include Rendering
  def hey = "hey"
end

class Direct < Base
  def send_action(m, *a) = [:direct, super]
end

b = Base.new
p b.send_action(:hi, 1)
p b.send_action(:kw, a: 1)
p b.send_action(:blk) { |x| x * 2 }
p b.send_action(:secret)
p b.dispatch_to(:hi, 2)
p b.call_public(:hi, 3)
begin
  b.call_public(:secret)
rescue NoMethodError => e
  puts e.message
end
p Controller.new.process(:hey)
p Controller.new.process(:hi, 4)
p Direct.new.process(:hi, 5)
p Base.instance_method(:send_action).arity
