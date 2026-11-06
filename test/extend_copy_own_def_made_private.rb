# the shadowing class method is made private: an explicit call raises, a bare one runs it
module M; def v = 1; end
class C; extend M; def self.t = v; end
puts C.t
class C
  def self.v = 2
  private_class_method :v
end
puts((C.v rescue $!.class))
puts C.t
