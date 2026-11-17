# A subclass's allocate whose super reaches a parent's allocate defined
# after the call: the call keeps Class#allocate's answer.
class P; attr_accessor :tag; end
class C < P; def self.allocate = super; end
class D < P; def self.allocate = (o = super; o); end
p C.allocate.tag
p D.allocate.class
p D.allocate.tag
class P; def self.allocate = (o = super; o.tag = :p; o); end
p C.allocate.tag
def mk = D.allocate
p mk.tag
p mk.class
