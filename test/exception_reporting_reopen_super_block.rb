class StandardError
  def message(&block)
    block.call if block
    "parent"
  end
end

class TypeError
  def message(&block) = super
end

p TypeError.new("seed").detailed_message
