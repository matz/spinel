class StandardError
  def message(&block)
    block.call if block
    "parent"
  end
end
class TypeError
  def message(&block) = super
end
def through(exception, &block)
  exception.message(&block)
end
err = TypeError.new("seed")
p through(err) { puts "forwarded" }
p err.message { puts "literal" }
p err.detailed_message
