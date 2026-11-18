class ReportText
  def to_str
    puts "convert"
    "disk is full"
  end
end

class Report < StandardError
  def message = ReportText.new
end

error = Report.new("seed")
p error.message.class
p error.detailed_message
