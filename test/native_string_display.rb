# spinel: gc-stress
class DisplayWord
  def initialize(value)
    @value = value
  end

  def to_s
    @value
  end

  def inspect
    @value
  end
end

class DisplayNumber
  def to_s
    42
  end

  def inspect
    42
  end
end

DisplayRecord = Struct.new(:word)
word = DisplayWord.new("one\0two")
puts "value=#{word}", word
print word, "\n"
p word
p DisplayRecord.new(word)
number = DisplayNumber.new
p "#{number}".start_with?("#<DisplayNumber:")
p number
NumberRecord = Struct.new(:word)
p NumberRecord.new(number)

class ComparisonWord
  def to_str
    "two"
  end
end

comparison = ComparisonWord.new
p "one" <=> comparison, "two".casecmp(comparison), "two".between?(comparison, comparison)
