class Parser
  SOFT_IDENTIFIER_KEYWORDS = %i[with]
  SHADOWED = %i[class]

  def soft?(value)
    SOFT_IDENTIFIER_KEYWORDS.include?(value)
  end

  def shadowed?(value)
    SHADOWED.include?(value)
  end
end

SHADOWED = %i[top]

puts Parser.new.soft?(:with)        # true
puts Parser.new.soft?(:without)     # false
puts Parser.new.shadowed?(:class)   # true
puts Parser.new.shadowed?(:top)     # false
