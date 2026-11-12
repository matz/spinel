# A local copied from a local that holds one class holds that class too, so
# a call on it reaches the class's own methods as a call on the constant does
kf = File
k2 = kf
p k2.basename("/a/b.rb", ".rb")
p k2.dirname("/a/b.rb")
ki = Integer
k3 = ki
k4 = k3
p k4.sqrt(17)
km = Math
k5 = km
p k5.sqrt(16.0)
# a long chain of copies holds the class at its head
c1 = File
c2 = c1
c3 = c2
c4 = c3
c5 = c4
c6 = c5
c7 = c6
p c7.extname("/a/b.rb")
# a cycle of copies holds the class coming into it
y1 = Integer
y2 = y1
y1 = y2
y3 = y1
p y3.sqrt(26)
# a copy in the chain written another class too holds neither, as CRuby
# reads the class that write last made
d1 = File
d2 = d1
d3 = d2
d3 = Dir if ARGV.empty?
d4 = d3
p d4.name
