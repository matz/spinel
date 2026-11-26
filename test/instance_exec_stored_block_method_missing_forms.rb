# A variable-like call in a stored block reaches the method_missing of the
# object instance_exec runs it on, however the block reaches instance_exec:
# a local, a Hash in the table, a method that stores it for another, a
# block a top-level method hands back, and an object of another class.
class Record
  def self.bodies = (@bodies ||= {})
  def self.define(name, block) = bodies[name] = block
  def self.body(name, &block) = define(name, block)
  def self.opts = (@opts ||= {})
  def self.with_opts(name, &block) = opts[name] = { body: block, tag: name }

  def initialize(attrs) = (@attrs = attrs)

  def method_missing(name, *args)
    return @attrs[name] if @attrs.key?(name)
    raise NoMethodError, "undefined method '#{name}'"
  end

  def run(name)
    b = self.class.bodies[name]
    self.instance_exec(&b)
  end

  def run_opt(name) = instance_exec(&self.class.opts[name][:body])

  body(:read) { token }
  with_opts(:other) { other }
end

r = Record.new({ token: "abc", other: 2 })
p r.run(:read)
p r.run_opt(:other)

class Ctx
  def initialize(args) = (@args = args)

  def method_missing(name, *args)
    return @args[name] if @args.key?(name)
    raise NoMethodError, "undefined method '#{name}'"
  end
end

def keep(&block) = block
body = keep { id }
p Ctx.new({ id: 1 }).instance_exec(&body)
