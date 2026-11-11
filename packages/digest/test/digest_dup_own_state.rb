# Digest::SHA256#dup and #clone keep their own state: updating the copy
# leaves the original as it was (#11348)
require "digest"
a = Digest::SHA256.new.update("hello ")
b = a.dup
b.update("world")
p b.hexdigest == Digest::SHA256.hexdigest("hello world")
p a.hexdigest == Digest::SHA256.hexdigest("hello ")
c = Digest::SHA256.new.update("hello ")
d = c.clone
d << "world"
p c.hexdigest == Digest::SHA256.hexdigest("hello ")
p d.hexdigest == Digest::SHA256.hexdigest("hello world")
m = Digest::MD5.new.update("x")
n = m.dup.update("y")
p m.hexdigest == Digest::MD5.hexdigest("x")
p n.hexdigest == Digest::MD5.hexdigest("xy")
