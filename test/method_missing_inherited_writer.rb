# An attr_accessor's writer answers an assignment on a class with a
# method_missing, on the class and on its subclasses.
class Base
  attr_accessor :route_names
  def method_missing(name, *args)
    return 1 if name.to_s.end_with?("_path")
    super
  end
  def respond_to_missing?(name, include_private = false)
    name.to_s.end_with?("_path") || super
  end
end
class Health < Base; end
class Rooms < Base; end

klass = [Health, Rooms][ARGV.size]
c = klass.new
p(c.route_names = [:a])
p c.route_names
p c.home_path

h = Health.new
h.route_names = [:h]
p h.route_names

b = Base.new
b.route_names = [:b]
p b.route_names
begin
  b.other = 1
rescue NoMethodError => e
  p e.name
end

class Guarded
  attr_reader :secret
  private attr_writer :secret
  def method_missing(name, *args) = "mm:#{name}"
  def respond_to_missing?(name, include_private = false) = true
  def set(v) = self.secret = v
end
class GuardedChild < Guarded; end
g = [Guarded, GuardedChild][ARGV.size].new
p(g.secret = 5)
p g.secret
g.set(6)
p g.secret
