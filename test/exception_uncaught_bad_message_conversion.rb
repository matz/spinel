class ReportLabel
  def to_str
    puts "converting"
    1
  end
end

class BadMessageError < StandardError
  def message = ReportLabel.new
end

puts "before"
raise BadMessageError, "stored"
