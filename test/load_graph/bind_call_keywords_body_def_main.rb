# an instance method a class method defines, read as an UnboundMethod and
# bound with keywords (`unbind.bind_call(obj, k:)`, `instance_method.bind_call`), before
# and after the body runs, on the class and a subclass.
def err(e = $!) = e.is_a?(NameError) ? [e.class, e.name] : e.class
class K
  def self.install
    def m(k: 1) = [:v1, k]
  end
  def self.install2
    def m(k: 2) = [:v2, k]
  end
end
class S < K; end
p((S.new.method(:m).unbind.bind_call(K.new, k: 3) rescue err))
K.install
p S.new.method(:m).unbind.bind_call(K.new, k: 3)
p K.instance_method(:m).bind_call(S.new, k: 4), K.new.m(k: 5)
K.install2
p K.new.m, S.new.m(k: 6)
