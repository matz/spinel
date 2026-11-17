# A `**`-only call of a builtin's name (Complex#phase) on a class held in a local or named by a constant path, where the program's method of the name requires keywords: the call without the operands raises CRuby's ArgumentError, and the one with them runs.
module Outer
  class Status
    attr_reader :phase, :attempt

    def initialize(phase, attempt)
      @phase = phase
      @attempt = attempt
    end

    def self.phase(phase:, attempt:)
      new(phase, attempt)
    end

    def line
      "#{phase} #{attempt}"
    end
  end
end

class Other
end

def pick(n) = n.zero? ? Other : Outer::Status

def try
  yield
rescue ArgumentError, NoMethodError => e
  "#{e.class}: #{e.message}"
end

full = { attempt: 1 }
empty = {}
puts Outer::Status.phase(**{ phase: "ready" }.merge(full)).line
puts try { Outer::Status.phase(**empty).line }
k = Outer::Status
puts k.phase(**{ phase: "built" }.merge(full)).line
puts try { k.phase(**empty).line }
include Outer
s = Status
puts s.phase(**{ phase: "local" }.merge(full)).line
puts try { s.phase(**empty) }
c = pick(1)
puts try { c.phase(**empty).line }
c = pick(0)
puts try { c.phase(**empty).line }.split(" for ").first

# a class whose method of the name cannot take the call, beside a class whose `class << self` accessor can
class Tagged
  def self.label(tag:) = 7

  def self.label=(v, tag:)
    @label = v
  end
end

class Named
  class << self
    attr_accessor :label
  end
end

def tag_pick(n) = n.zero? ? Tagged : Named

t = tag_pick(1)
p(t.label = "named")
p t.label
t = tag_pick(0)
puts try { t.label }
begin
  t.label = 3
rescue ArgumentError => e
  puts "#{e.class}: #{e.message}"
end

# a writer called by send or public_send, where no class method of the name takes the call
class Counted
  def self.count=(a, b)
    @count = a
    0
  end
end

class Kept
  class << self
    attr_accessor :count
  end
end

def count_pick(n) = n.zero? ? Counted : Kept

w = count_pick(1)
p w.send(:count=, 7)
p w.public_send(:count=, 8)
p Kept.count
w = count_pick(0)
begin
  w.send(:count=, 7)
rescue ArgumentError => e
  puts "#{e.class}: #{e.message}"
end
