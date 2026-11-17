# A subclass's allocate whose super reaches a parent's allocate that is
# reopened after the call: the call meets the parent's version in force
# there, not the later one. (Run in the promote lane: in the default one
# an Exception subclass's own allocate does not compile yet.)
class P < StandardError; def self.allocate = (o = super; o); end
class C < P; def self.allocate = (o = super; o); end
p C.allocate.class
class P; def self.allocate = C.exception("p2"); end
p C.allocate.message
