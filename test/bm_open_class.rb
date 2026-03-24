# Test: open class support for built-in types
# Extension methods on String, Integer, Float

class String
  def shout
    upcase + "!"
  end

  def whisper
    downcase + "..."
  end
end

class Integer
  def double
    self * 2
  end

  def square
    self * self
  end
end

puts "hello".shout
puts "HELLO WORLD".whisper
puts 5.double
puts 7.square
