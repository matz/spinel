# A visibility word in a `class << self` that a class method opens names
# the class's singleton methods, not its instance methods: an instance
# method of the same name keeps its visibility after the word runs. A
# word a class method runs on self itself names the instance methods.
class H
  def m = :im
  def self.m = :cm
  def self.close
    class << self
      private :m
    end
  end
end
class G
  def i = :i
  def self.hide_i
    private :i
  end
end
def chk(tag)
  h = H.new
  g = G.new
  p [tag, (h.m rescue $!.class), h.respond_to?(:m), H.public_method_defined?(:m), H.respond_to?(:m, true)]
  p [tag, (g.i rescue $!.class), g.respond_to?(:i), G.private_method_defined?(:i)]
end
chk(:before)
H.close
chk(:closed)
G.hide_i
chk(:after)
