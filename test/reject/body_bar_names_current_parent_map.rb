# spinel: reject-subclass: where a method_added or singleton_method_added hook of the program may observe it
# The walk's index of the entries that bar a body's version keeps the names it read, with the parent map it read them with.
# A program with hooks and a class builder appends nodes while the walk runs, which replaces the map; a bar asked again after
# that must read the current one. The program is refused as before (an accessor in a method body, with a hook of the program
# that may observe it), and the compile reads no freed map.
class C
  def self.mkc = define_method(:a) { 1 }
end
C.mkc
class Hk
  def self.method_added(n) = nil
  def self.inherited(k) = nil
end
class D < Hk
  def b = :def
  def self.mk
    attr_accessor(:b)
    nil
  end
  def self.hide
    private(:b)
    nil
  end
end
d = D.new
D.mk
d.b = 2
p d.b
Kls = Class.new(Hk) do
  def self.mk2
    attr_accessor(:z)
    nil
  end
end
Kls.mk2
module Ext
  def self.included(k) = nil
  def self.extended(k) = nil
end
class E < Hk
  include Ext
  extend Ext
  def self.mk3
    attr_accessor(:b)
    nil
  end
end
D.hide
p((d.b rescue :nme_private))
