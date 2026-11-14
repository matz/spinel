# A class method that passes an instance through and builds one from
# anything else: `return text if text.is_a?(self)` answers only an
# instance of the class, so a String parameter does not contradict the
# seeded return (the return never runs for a String).
module IsaGuardNet
  class Link
    def self.parse(text)
      return text if text.is_a?(self)

      IsaGuardNet::Link.new(text)
    end

    def self.coerce(text)
      return text if text.is_a?(IsaGuardNet::Link)

      IsaGuardNet::Link.new(text)
    end

    def initialize(text)
      @text = text
    end

    def text
      @text
    end
  end
end

puts IsaGuardNet::Link.parse("a").text
puts IsaGuardNet::Link.coerce("b").text
