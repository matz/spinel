# A Comparable method may call another of its methods with no receiver.
module Comparable
  def tag = "b"
  def tagged = tag + "?"
end
puts "x".tagged
puts :s.tagged
puts 3.tagged
