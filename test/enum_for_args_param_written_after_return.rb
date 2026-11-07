# `return enum_for(:m, args) unless block_given?` where the method writes
# a parameter it forwards after that return: the write never runs in the
# frame that made the Enumerator, so the Enumerator calls m with the
# arguments it was given.

module Pairs
  def self.each_pair(s, separator = nil)
    return enum_for(:each_pair, s, separator) unless block_given?
    s ||= ""
    s.split(separator || "&").each do |part|
      next if part.empty?
      k, v = part.split("=", 2)
      yield k, v
    end
  end
end

def collect(pairs)
  h = {}
  pairs.each { |k, v| h[k] = v }
  h
end

p collect(Pairs.each_pair("a=1&b=2"))
p collect(Pairs.each_pair("c=3;d;", ";"))
p Pairs.each_pair(nil).to_a
Pairs.each_pair("x=9") { |k, v| p [k, v] }

class Words
  def initialize(text) = @text = text

  def each_word(min, *skip)
    return enum_for(:each_word, min, *skip) unless block_given?
    min = min.to_i
    skip = skip.map(&:downcase)
    @text.split.each { |w| yield w if w.length >= min && !skip.include?(w.downcase) }
  end
end

w = Words.new("The quick brown fox jumps over the lazy dog")
p w.each_word(4).to_a
p w.each_word("3", "THE", "fox").map(&:upcase)
e = w.each_word(5)
p e.next
p e.next
