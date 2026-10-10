# One call site runs both String#sub and same-named user methods. Its user
# arms return nil, a fresh String, a borrowed String, or a container holding
# a borrowed String; those return facts must remain intact beside the builtin.
# spinel: share
class ParamsNil
  def sub(pattern, replacement)
    nil
  end
end

class ParamsFresh
  def sub(pattern, replacement)
    String.new("fresh")
  end
end

class ParamsBorrowed
  attr_reader :returned

  def initialize
    @returned = String.new("borrowed")
  end

  def sub(pattern, replacement)
    @returned
  end
end

class ParamsContainer
  attr_reader :returned

  def initialize
    @returned = String.new("container")
  end

  def sub(pattern, replacement)
    [@returned]
  end
end

def invoke_sub(receiver)
  receiver.sub("://", "!")
end

source = Thread.new { "https://" }.value
string_result = invoke_sub(source)
string_result << "!"
puts source
puts string_result

puts invoke_sub(ParamsNil.new).nil?

fresh_result = invoke_sub(ParamsFresh.new)
fresh_result << "!"
puts fresh_result

borrowed = ParamsBorrowed.new
borrowed_result = invoke_sub(borrowed)
borrowed_result << "!"
puts borrowed.returned

container = ParamsContainer.new
container_result = invoke_sub(container)
container_result[0] << "!"
puts container.returned
