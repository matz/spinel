# A boxed typed Array returned through Array[untyped] needs its elements
# converted. Reading the typed Array's pointer as a PolyArray lost them.
require "json"

module PreparedCommand
  def self.command(original, wrap)
    return original unless wrap
    ["bash", *original]
  end

  def self.tail(value)
    value
  end

  def self.nullable(value)
    value
  end

  def self.rescued(value)
    begin
      return value
    rescue ArgumentError
      ["fallback", 1]
    end
  end

  def self.ensured(value)
    begin
      return value
    ensure
      puts "ensured"
    end
  end
end

p JSON.generate({ "command" => PreparedCommand.command(["bash", "-lc", "echo ok"], false) })
p PreparedCommand.command(["sh", "-l"], true)
p PreparedCommand.tail([7, 19])
p PreparedCommand.tail([1.25, 2.5])
p PreparedCommand.tail(["one", "two"])
p PreparedCommand.tail(["mixed", 23])
p PreparedCommand.tail([])
p PreparedCommand.nullable(nil)
p PreparedCommand.nullable(["nullable", "value"])
p PreparedCommand.rescued(["rescue", "value"])
p PreparedCommand.ensured(["ensure", "value"])
