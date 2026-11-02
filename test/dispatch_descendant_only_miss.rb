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
