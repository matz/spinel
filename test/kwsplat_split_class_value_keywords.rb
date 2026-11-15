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
