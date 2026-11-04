# A Time maker on a Class value makes a Time subclass's Time when the
# subclass is namespaced (tzinfo's `new_time(TZInfo::TimeWithOffset)`).
module NS
  class Shifted < Time
    def shift_to(offset)
      localtime(offset)
      @offset = offset
      self
    end

    def offset
      @offset
    end
  end

  class Stamp
    def initialize(value)
      @value = value
    end

    def to_time
      new_time.localtime(3600)
    end

    def new_time(klass = Time)
      klass.at(@value)
    end
    protected :new_time
  end

  class ShiftedStamp < Stamp
    def to_time
      new_time(NS::Shifted).shift_to(7200)
    end
  end
end

puts NS::Stamp.new(0).to_time.utc_offset
t = NS::ShiftedStamp.new(0).to_time
puts t.utc_offset
puts t.offset
puts t.class
puts t.is_a?(Time)
