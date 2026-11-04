# A Range of Times held where its kind is known only at run time (a Hash
# value, an Array element, an ivar) answers the same methods boxed.
now = Time.utc(2026, 11, 4, 12, 0, 0)
day = (now - 86400)..now
puts "window: #{day}"
h = { w: day, n: 3 }
p h[:w].begin
p h[:w].end.hour
p h[:w].exclude_end?
p h[:w].include?(now - 3600)
p h[:w] == day
p h[:w].to_s
xs = [day, (now..(now + 60))]
xs.each { |r| p r.cover?(now) }
p xs.map(&:begin).map(&:day)
class Report
  def initialize(range)
    @range = range
  end
  def covers?(t) = @range.cover?(t)
  def span = @range
  def width = @range.end - @range.begin
end
rep = Report.new(day)
p rep.covers?(now - 10), rep.covers?(now + 10)
p rep.width
p rep.span
def month_of(t)
  first = Time.utc(t.year, t.month, 1)
  last = t.month == 12 ? Time.utc(t.year + 1, 1, 1) : Time.utc(t.year, t.month + 1, 1)
  first...last
end
m = month_of(now)
p m, m.cover?(now)
opt = ARGV.empty? ? m : nil
p opt&.begin
p opt.nil?
p m.eql?(month_of(now)), m.equal?(m)
p({ m => 1 }[month_of(now)])
p m.is_a?(Range), m.kind_of?(Enumerable), m.respond_to?(:cover?)
