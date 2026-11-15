# A new on a class value some of whose classes have a program `new` of their own runs, for the others, the initialize version their arm names where the site runs, and the version is kept reachable.
class A; attr_reader :v; def initialize(x) = @v = [:a1, x]; end
class B
  attr_reader :v
  def self.new(x) = super(x)
  def initialize(x) = @v = [:b, x]
end
k = ARGV.empty? ? A : B
p k.new(1).v
class A; def initialize(x) = @v = [:a2, x]; end
p k.new(2).v

class A2; attr_reader :v; def initialize(x) = @v = x + 1; end
class B2
  attr_reader :v
  def self.new(x) = super(x)
  def initialize(x) = @v = x
end
k2 = ARGV.empty? ? A2 : B2
p k2.new(1).v
class A2; def initialize(x) = @v = x.to_s * 2; end
p k2.new("z").v
