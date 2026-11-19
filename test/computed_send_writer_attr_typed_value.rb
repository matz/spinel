# A computed-name send of a writer (`o.public_send("#{name}=", v)`) with a
# value of one type, where a subclass's attr_accessor overrides an
# ancestor's `def x=`: the attr writer answers for that class, as it does
# when the value is boxed (computed_send_writer_attr_override.rb).
class Base
  def logger=(v); @log = v; end
  def logger; @log; end
  def ratio=(v); @r = v; end
  def ratio; @r; end
end
class Mid < Base; end
class Leaf < Base
  attr_accessor :logger, :ratio
end

objs = [Base.new, Mid.new, Leaf.new]
objs.each do |o|
  name = "logger"
  r = o.public_send("#{name}=", 7)
  p [r, o.logger]
end
leaf = Leaf.new
leaf.public_send("log#{'ger'}=", 3 + ARGV.size)
p leaf.logger, leaf.instance_variable_get(:@log)
