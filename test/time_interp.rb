# Time values in string interpolation — routed through Time#to_s
# (whole seconds, no fractional part), same as the explicit call.
t = Time.at(0).utc
puts "at '#{t}'"
puts "at '#{t.to_s}'"

# fractional seconds drop in to_s (inspect would keep them)
f = Time.at(1234567890.5).utc
puts "frac #{f}"

class Report
  def initialize
    @period = Time.at(86400).utc
  end

  def period
    @period
  end

  def sql
    "
      where (created_at >= '#{period}')
    "
  end
end

puts Report.new.sql
