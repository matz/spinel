module M
  String = 5
  def self.a = ::String
  def self.b = Object.const_get(:String)
  def self.k = Kernel.const_get(:String)
  def self.c = String
end
p M.a
p M.b
p M.k
p M.c
p M::String
p String

module B
  Array = [1]
  Hash = 2
  def self.a = Array
  def self.h = ::Hash.new.class
end
p B.a
p B.h
p Array.new(2, 0)

class C
  Integer = "i"
  def i = Integer
  def j = ::Integer
end
p C.new.i
p C.new.j
p 3.is_a?(Integer)
