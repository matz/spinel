# attr_* in a class body's loop over literal names defines each one, as
# define_method in the same loop does
class Conn
  [:user, :device].each { |name| attr_accessor name }
  %w[token].each { |name| attr_reader name }
  [:note].each { |name| attr_writer name }
  def initialize = @token = "t"
end

c = Conn.new
c.user = 1
c.device = "ios"
c.note = "n"
p [c.user, c.device, c.token]
p Conn.public_method_defined?(:note=)

# a class method that the compiler runs with a literal or a computed name
class Base
  def self.idb(name) = attr_accessor(name)
end

class Sub < Base
  idb :user
  idb(("dev" + "ice").to_sym)
end

s = Sub.new
s.user = 2
s.device = "web"
p [s.user, s.device]

# attr with its trailing flag
class Flag
  attr :x, false
  def initialize = @x = 3
end

p Flag.new.x

# a class body whose value is read
result = class Kept
  [:x].each { |name| attr_accessor name }
end

kept = Kept.new
kept.x = 4
p result, kept.x
