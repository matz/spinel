# the compile-time macro runner runs a loop over an Array the frame knows:
# attr_* in each or each_with_index of a class method called with literals
# (ActionCable's identified_by), and leaves a loop it cannot follow as written
class Conn
  def self.identified_by(*identifiers)
    Array(identifiers).each { |identifier| attr_accessor identifier }
  end
  identified_by :current_user, :room
end

class Literal
  def self.pair(*names) = names.each { |name| attr_accessor name }
  pair :x, :y
end

class Pair
  def self.both = %i[left right].each { |n| attr_reader n }
  def self.indexed(*ns) = ns.each_with_index { |n, i| attr_writer "#{n}#{i}" }
  both
  indexed :a, :b
  def initialize = (@left, @right = 1, 2)
end

class Seen
  @seen = []
  class << self
    attr_reader :seen
  end
  def self.track(*names) = names.each { |n| @seen << n }
  track :x, :y
end

class Shadow
  def self.pick(n, *rest)
    rest.each { |n| attr_accessor n }
    attr_reader n
  end
  pick :first, :second, :third
end

c = Conn.new
c.current_user = "u"
c.room = 7
p [c.current_user, c.room]
l = Literal.new
l.x = 1
p [l.x, l.respond_to?(:y=)]
pr = Pair.new
pr.a0 = 3
p [pr.left, pr.right, pr.respond_to?(:a0=), pr.respond_to?(:b1=), pr.respond_to?(:a=)]
p Seen.seen
s = Shadow.new
s.second = 2
p [s.respond_to?(:first), s.respond_to?(:first=), s.second, s.respond_to?(:third=)]

# the macro inherited from a superclass, run for the subclass's body
class Base
  def self.identified_by_names(*names)
    names.each { |name| attr_accessor name }
  end
end

class Device < Base
  identified_by_names :user, :device
end

d = Device.new
d.user = 1
p [d.user, Base.new.respond_to?(:user)]
