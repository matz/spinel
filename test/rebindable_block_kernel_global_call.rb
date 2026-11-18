# A block a user method takes as a Proc may be instance_exec'd, so its self
# uses are read: a receiverless call to a Kernel function (puts, raise)
# dispatches the same on any self, so it is not one, even when a class of the
# program defines a method of that name.

# a method that keeps the block it is given
class Registry
  def initialize = @blocks = {}
  def define_method(name, &block) = @blocks[name] = block
  def run(name) = @blocks[name].call
end

# a class whose instance methods share a Kernel function's name
class Console
  def puts(msg) = Kernel.puts("console: #{msg}")
  def raise(msg) = Kernel.puts("console raise: #{msg}")
end

# a block instance_exec'd through a stored Proc: the program has rebindable literals
class Ctx
  def initialize(n, &b) = (@n = n; @blk = b)
  def go(other) = other.instance_exec(&@blk)
  def n = @n
end

def install(reg, names)
  names.each do |m|
    reg.define_method(m) { puts "called #{m}" }
  end
  reg.define_method(:boom) { raise "boom" }
end

r = Registry.new
install(r, [:a, :b])
r.run(:a)
r.run(:b)
begin
  r.run(:boom)
rescue RuntimeError => e
  puts "raised #{e.message}"
end
Console.new.puts("direct")
p Ctx.new(5) { n + 1 }.go(Ctx.new(9))
