# Kernel#p evaluates its arguments, then calls inspect and writes one
# argument at a time, so an inspect runs before the write raises FrozenError
out = STDOUT.dup
out.sync = true
$stdout.sync = true

class Demo
  def inspect
    @x = 2
    "demo"
  end

  def run
    p self, (@x = 1), @x
  end
end
Demo.new.run

class Freezer
  def inspect
    STDOUT.freeze
    "freezer"
  end
end

class Boom
  def inspect
    $seen = 1
    raise ArgumentError, "boom"
  end
end

items = [Freezer.new]
begin
  p(*items)
  out.puts "wrote"
rescue FrozenError => e
  out.puts e.message
end

$seen = 0
booms = [Boom.new]
begin
  p(*booms)
  out.puts "wrote"
rescue FrozenError, ArgumentError => e
  out.puts "#{e.class} #{$seen}"
end

$seen = 0
begin
  r = p(*booms)
  out.puts r.inspect
rescue FrozenError, ArgumentError => e
  out.puts "#{e.class} #{$seen}"
end

$seen = 0
begin
  p 1, Boom.new
  out.puts "wrote"
rescue FrozenError, ArgumentError => e
  out.puts "#{e.class} #{$seen}"
end
