# A class's own self.exception that calls super with a splat reaches
# Exception.exception, which builds the class through new.
def show; yield; rescue Exception => e; p [e.class, e.message]; end
class B < StandardError
  def self.exception(*a)
    puts "B.exception #{a.inspect}"
    super(*a.map(&:upcase))
  end
end
show { raise B, "bb" }
show { raise B }
show { e = B.exception("x", "y"); p e }
class W < StandardError
  def initialize(a, b = "dflt") = super("#{a}/#{b}")
  def self.exception(*a) = super(*a.reverse)
end
show { raise W, "w" }
show { e = W.exception("1", "2"); p e.message }
class S < SystemExit
  def self.exception(*a) = super(*a)
end
show { raise S, "bye" }
