module M; def v = 1; end
class C; extend M; end
def g = C.v
