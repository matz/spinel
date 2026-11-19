# A call or a respond_to? at a point the load fixes, before or after a
# class method whose body runs a visibility word, reads the visibility the
# method has there: public before the word runs, private after.
class K
  def m = :m
end
class K
  def self.hide
    private :m
  end
end
k = K.new
p [(k.m rescue $!.class), k.respond_to?(:m)]
K.hide
p [(k.m rescue $!.class), k.respond_to?(:m)]
class J
  def self.setup
    def m = :j
  end
  def self.hide
    private :m
  end
end
j = J.new
xs = [j, 1]
p [j.respond_to?(:m), xs.map { |x| x.respond_to?(:m) }]
J.setup
p [j.respond_to?(:m), xs.map { |x| x.respond_to?(:m) ? x.m : :none }]
J.hide
p [j.respond_to?(:m), xs.map { |x| x.respond_to?(:m) ? x.m : :none }, (j.m rescue $!.class)]
