# two extends, then an own def whose super reaches the later module only
module M1; def v = 1; end
module M2; def v = 3; end
class C; extend M1; end
puts C.v
class C; extend M2; end
puts C.v
def C.v = super * 10
puts C.v
