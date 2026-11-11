# A super in == reaches a == the program defines on BasicObject.
class BasicObject
  def ==(o) = :basic_object
end
class A
  def ==(o) = super
end
p(A.new == 1)
