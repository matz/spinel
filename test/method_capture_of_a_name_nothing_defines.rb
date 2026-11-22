# `recv.method(:name)` for a name no object has (no core class has it, and
# the program defines it nowhere) is CRuby's NameError at the capture, naming
# the receiver's class. A plain Object (also boxed), main, and a class whose
# capture runs in a block made a Method instead, whose call raised
# NoMethodError later.
def capture(label)
  m = yield
  puts "#{label}: #{m.inspect}"
rescue NameError => e
  puts "#{label}: #{e.class}: #{e.message} (name: #{e.name.inspect})"
end

class Foo
  def bar = 1
end

capture("Object.new") { Object.new.method(:missing) }
capture("Foo in a block") { Foo.method(:nope) }
[Object.new, "s", 1, nil].each do |v|
  capture("boxed #{v.class}") { v.method(:zork) }
end

begin
  method(:missing)
rescue NameError => e
  puts "main: #{e.class} (name: #{e.name.inspect})"
end

# a name that exists is still captured
p Foo.new.method(:bar).call
p [Object.new, Foo.new].map { |v| v.method(:frozen?).call }
