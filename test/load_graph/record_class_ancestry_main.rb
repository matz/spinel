# Which classes can be on another's chain (--dump-load-record,
# record_class_ancestry_main.rb.load-record, "seen="). A class is on a
# chain only through a superclass, whatever the chain mixes in: A includes
# M, which includes a module the record cannot read, so a module may be on
# A's chain, but class K may not be. Code the record cannot follow, run on
# K between two versions of A#v, does not move A's chain (seen=def#8); run
# on module N it may (the intervals it falls in are seen=unknown), and so
# is code run on a reopened Object, which every class's chain ends in
# (A3#v), though no superclass walk meets it.
module M
  include Comparable
  def <=>(o) = 0
end
class K; end
module N; end
class A
  include M
  def v = 1
  def w = 1
end
puts A.new.v
class K; send(:public); end
class A
  def v = 2
end
puts A.new.v
puts A.new.w
module N; send(:public); end
class A
  def w = 2
end
puts A.new.w

class A3
  include M
  def v = 1
end
puts A3.new.v
class Object; send(:public); end
class A3
  def v = 2
end
puts A3.new.v
