# A Hash subclass whose initialize takes *args passes them to Hash's
# initialize with super: no argument, a default value, or a default block
# (net-ldap's Dataset < Hash)
class Bare < Hash
  def initialize(*args, &block)
    super
  end
end

class Spread < Hash
  def initialize(*args, &block)
    super(*args, &block)
  end
end

class Rest < Hash
  def initialize(*args)
    super
  end
end

p Bare.new
p Bare.new(5)[:x]
p Bare.new { |h, k| "#{k}!" }[:y]
p Spread.new
p Spread.new(7)[:x]
p Spread.new { |h, k| k.to_s * 2 }[:ab]
p Rest.new
p Rest.new(:none)[:x]
b = Bare.new
b[:k] = 1
p b, b.class
begin
  Bare.new(1, 2)
rescue ArgumentError => e
  puts e.message
end

# entries written ahead of the super stay, and the super calls no replace
class Seeded < Hash
  def initialize(*args, &block)
    self[:seed] = 1
    super
  end
end
class OwnReplace < Hash
  def replace(other) = raise("replace called")
  def initialize(*args, &block)
    super
  end
end
s = Seeded.new(0)
p s, s[:x]
t = Seeded.new { |h, k| h[k] = k.to_s }
p t[:z], t
p OwnReplace.new(5)[:y], OwnReplace.new[:y]

# the super sets the default as Hash#initialize does, through no override
class OwnSetters < Hash
  def default=(v)
    raise "own default="
  end
  def default_proc=(v)
    raise "own default_proc="
  end
  def initialize(*args, &block)
    super
  end
end
class OwnDefault < Hash
  def default=(v)
    raise "own default="
  end
  def initialize(d)
    super
  end
end
p OwnSetters.new(5)[:x], OwnSetters.new { |h, k| k }[:y], OwnSetters.new[:z], OwnDefault.new(3)[:w]
