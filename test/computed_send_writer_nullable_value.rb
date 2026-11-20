# A computed-name send of a writer whose value is an Integer that can be nil:
# the plan and the emitter must type the value alike, or the subclass whose
# attr_accessor overrides an inherited writer gets no arm.
class Base
  def logger=(v); @log = v; end
  def logger; @log; end
end
class Mid < Base; end
class Leaf < Base
  attr_accessor :logger
end

v = ARGV.empty? ? 7 : nil
[Base.new, Mid.new, Leaf.new].each do |o|
  name = "logger"
  r = o.public_send("#{name}=", v)
  p [r, o.logger]
end
leaf = Leaf.new
leaf.public_send("log#{'ger'}=", ARGV.size > 3 ? nil : 3)
p leaf.logger, leaf.instance_variable_get(:@log)
