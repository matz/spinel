# An undef on main's singleton class stops the method lookup there: a call
# on main raises, though Object, a top-level def or the core (main's own
# to_s and inspect) has a method of the name behind it. A removal passes the
# lookup on to Object's method, however many versions main's method had.

class Object
  def f1 = "Object#f1"
  def f2(x) = "Object#f2 #{x}"
  def f3 = "Object#f3"
  def f4 = "Object#f4"
  def f5 = "Object#f5"
  def f6 = "Object#f6"
end

def self.f1 = "main f1"
puts f1
class << self
  undef_method :f1
end
begin
  f1
rescue NameError => e
  puts "#{e.class} #{e.name}"
end
begin
  self.f1
rescue NoMethodError => e
  puts "#{e.class} #{e.name} #{e.receiver.inspect}"
end
puts [1].map { f1 rescue "NameError in a block" }.inspect
def self.f1 = "main f1 again"
puts f1

# with an argument, and by the undef keyword
def self.f2(x) = "main f2 #{x}"
puts f2(1)
class << self
  undef_method :f2
end
begin
  f2(2)
rescue NoMethodError => e
  puts "#{e.class} #{e.name}"
end
def self.f3 = "main f3"
class << self
  undef f3
end
puts((f3 rescue "NameError f3"))

# main's own method was never defined, or is a top-level def
class << self
  undef_method :f4
end
puts((f4 rescue "NameError f4"))
puts nil.f4
def g1 = "top g1"
def self.g1 = "main g1"
class << self
  undef_method :g1
end
puts((g1 rescue "NameError g1"))

# the core's to_s and inspect
class << self
  undef_method :to_s
  undef_method :inspect
end
puts((to_s rescue "NameError to_s"))
p((to_s rescue :NameError))
p((inspect rescue :NameError))
p((self.inspect rescue :NoMethodError))

# past eight versions of main's method: an undef, a removal
def self.f5 = "main f5 1"
def self.f5 = "main f5 2"
def self.f5 = "main f5 3"
def self.f5 = "main f5 4"
def self.f5 = "main f5 5"
def self.f5 = "main f5 6"
def self.f5 = "main f5 7"
def self.f5 = "main f5 8"
def self.f5 = "main f5 9"
puts f5
class << self
  undef_method :f5
end
puts((f5 rescue "NameError f5"))
def self.f6 = "main f6 1"
def self.f6 = "main f6 2"
def self.f6 = "main f6 3"
def self.f6 = "main f6 4"
def self.f6 = "main f6 5"
def self.f6 = "main f6 6"
def self.f6 = "main f6 7"
def self.f6 = "main f6 8"
def self.f6 = "main f6 9"
puts f6
class << self
  remove_method :f6
end
puts f6
