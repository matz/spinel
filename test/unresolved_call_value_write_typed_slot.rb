# An unresolved call written through an instance, class or global variable
# in value position (`f = @h = a.index_with { nil }`), into a slot whose
# other writes type it: the call's value is the gate's boxed NoMethodError
# raise, and the write put that box into the Hash, Integer or object slot
# unconverted, so the C did not build. The raise runs first, as in CRuby.
RULES = { "a" => "x", "b" => "y" }

def report
  yield
rescue NoMethodError => e
  puts "NoMethodError: #{e.message[/undefined method '[^']+'/]}"
end

class Point
  def to_s = "point"
end

class Inspection
  @@cache = { "c" => 1 }

  def initialize
    @facts = { "start" => 1 }
    @count = 1
    @point = Point.new
  end

  def facts
    f = @facts = RULES.keys.index_with { nil }
    f["stage"] = 2
    f
  end

  def count
    n = @count = RULES.keys.index_with { nil }
    n + 1
  end

  def point
    o = @point = RULES.keys.index_with { nil }
    o.to_s
  end

  def self.cache
    f = @@cache = RULES.keys.index_with { nil }
    f
  end

  def start = @facts
end

$seen = { "s" => 1 }

def seen
  f = $seen = RULES.keys.index_with { nil }
  f
end

i = Inspection.new
report { p i.facts }
report { p i.count }
report { p i.point }
report { p Inspection.cache }
report { p seen }
p i.start
p $seen
