# A receiverless call in an instance method whose class has no method of its
# name, only a subclass or a singleton definition does, dispatches on self's
# class; self of a class without the method raises NameError for a bare name
# and NoMethodError otherwise, as in CRuby, naming self's class.

class Base
  def run = step
  def run_paren = step()
  def count = tally + 1
end
class Sub < Base
  def step = "sub step"
  def tally = 41
end
class Other < Base; end

def show
  yield
rescue NameError => e
  "#{e.class}: #{e.message}"
end

p Sub.new.run, Sub.new.run_paren, Sub.new.count
puts show { Base.new.run }
puts show { Base.new.run_paren }
puts show { Other.new.run }
puts show { Base.new.count }

# the method only one object has
class Foo
  def call_hi = hi
end
o = Foo.new
def o.hi = "tw"
p o.call_hi
puts show { Foo.new.call_hi }

class Bar
  def call_ho = ho
end
b = Bar.new
b.define_singleton_method(:ho) { "ho" }
p b.call_ho
puts show { Bar.new.call_ho }

# the error carries self as its receiver
x = Other.new
p((begin; x.run; rescue NameError => e; [e.name, e.receiver.equal?(x)]; end))
p((begin; x.run_paren; rescue NoMethodError => e; [e.name, e.receiver.equal?(x)]; end))

# a count the subclass's method refuses: its arm raises ArgumentError, and
# self of a class without the method still reaches the miss
class Walk
  def none = pace()
  def two = pace(1, 2)
  def kw = pace(1, k: 2)
end
class Stroll < Walk
  def pace(x) = x + 1
end
[Stroll.new, Walk.new].each do |w|
  %i[none two kw].each do |m|
    puts((begin; w.send(m); rescue ArgumentError, NoMethodError => e; "#{e.class}: #{e.message}"; end))
  end
end

# arms that take the arguments differently run them in each arm; self of a
# class without the method runs them too before the miss
$log = []
def tick = ($log << :tick; 1)
def arr = ($log << :arr; [1])
def pr = ($log << :pr; proc { 1 })
class Hop
  def a = jump(tick)
  def b = jump(*arr)
  def c = jump(1, &pr)
end
class Skip < Hop
  def jump(x, &blk) = x
end
class Leap < Hop
  def jump(x, y = 2, &blk) = x + y
end
[Skip.new, Leap.new, Hop.new].each do |h|
  %i[a b c].each do |m|
    puts((begin; h.send(m).to_s; rescue NoMethodError => e; e.class.to_s; end))
  end
end
p $log

# a subclass that answers through an accessor keeps CRuby's ArgumentError
class Gauge
  def read = level(1)
  def write = send(:level=)
end
class Dial < Gauge
  attr_accessor :level
end
class Knob < Gauge
  def level = 7
  def level=(x)
    @level = x
  end
end
[Dial.new, Knob.new].each do |g|
  %i[read write].each do |m|
    puts((begin; g.send(m); rescue ArgumentError => e; "#{e.class}: #{e.message}"; end))
  end
end
