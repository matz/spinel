p Array[1, 2]
class Array
  def self.[](*a) = [:own, *a]
end
p Array[3, 4]
