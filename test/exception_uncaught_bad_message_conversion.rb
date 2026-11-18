class ReportLabel
  def to_str = 1
end

class BadMessageError < StandardError
  def message = ReportLabel.new
end

puts "before"
raise BadMessageError, "stored"
