# A method the program defines in Range has a body for an Integer Range
# alone, so calling one on a Range of Times is refused, as it is on a Range
# of Strings or Floats.
class Range
  def span_s = "#{self.begin}|#{self.end}"
end
a = Time.utc(2026, 1, 1)
b = Time.utc(2026, 2, 1)
puts (a..b).span_s
