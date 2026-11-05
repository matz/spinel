# a subclass reads the parent's extended method; the parent shadows it later
module M; def v = 1; end
class C; extend M; end
class D < C; end
puts D.v
def C.v = 2
puts D.v
