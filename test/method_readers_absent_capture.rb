# every Method reader of a capture that raises (no method of the name where it
# runs): before a body defines it, and of a name nothing defines. The capture's NameError,
# the reader never runs.
def err(e = $!) = e.is_a?(NameError) ? [e.class, e.name] : e.class
class K
  L = lambda { |n|
    def self.m(a) = [a, :m]
  }
end
class N; end
p((K.method(:m).arity rescue err), (K.method(:m).owner rescue err), (K.method(:m).parameters rescue err))
p((K.method(:m).source_location rescue err), (K.method(:m).receiver rescue err), (K.method(:m).unbind rescue err))
p((K.method(:m).original_name rescue err), (K.method(:m).name rescue err), (K.method(:m).call(1) rescue err))
p((N.method(:zz).owner rescue err), (N.method(:zz).arity rescue err))
K::L.call(1)
p K.method(:m).arity, K.method(:m).owner, K.method(:m).call(2)
