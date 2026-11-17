fields = [2024, 1, 2]
puts Time.new(*fields).year
class Time
  def self.new(*a) = Time.utc(2030)
end
puts Time.new(*fields).year
